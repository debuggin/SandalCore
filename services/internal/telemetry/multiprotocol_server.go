// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package telemetry

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"golang.org/x/sync/errgroup"
)

const (
	defaultMaxConcurrentStreams = 250
	defaultReadHeaderTimeout    = 5 * time.Second
	defaultRWTimeout            = 15 * time.Second
	defaultIdleTimeout          = 60 * time.Second
	defaultShutdownTimeout      = 5 * time.Second
)

type MultiProtocolConfig struct {
	HTTPBindAddr, HTTPSBindAddr, QUICBindAddr string
	TLSCertFile, TLSKeyFile, AltSvcHeader     string
	TLSConfig                                 *tls.Config
	MaxConcurrentStreams                      uint32
	DisableQUIC                               bool
}

type MultiProtocolServer struct {
	httpLn, httpsLn      net.Listener
	quicConn             net.PacketConn
	cleartextSrv, tlsSrv *http.Server
	tlsCfg               *tls.Config
	altSvc               string
	closeOnce            sync.Once
}

func ResolveDualStackAddrs(ctx context.Context, hostPort string) ([]netip.AddrPort, error) {
	host, portStr, err := net.SplitHostPort(hostPort)
	port64, pErr := strconv.ParseUint(portStr, 10, 16)
	if host = strings.TrimSpace(host); err != nil || pErr != nil || host == "" || port64 == 0 {
		return nil, fmt.Errorf("invalid host/port %q", hostPort)
	}
	ips, err := net.DefaultResolver.LookupNetIP(ctx, "ip", host)
	if err != nil {
		return nil, fmt.Errorf("lookup host %q: %w", host, err)
	}
	var v6, v4 []netip.AddrPort
	seen := make(map[netip.AddrPort]struct{}, len(ips))
	for _, raw := range ips {
		ip := raw.Unmap()
		ap := netip.AddrPortFrom(ip, uint16(port64))
		if _, dup := seen[ap]; !dup {
			seen[ap] = struct{}{}
			if ip.Is6() {
				v6 = append(v6, ap)
			} else if ip.Is4() {
				v4 = append(v4, ap)
			}
		}
	}
	if out := append(v6, v4...); len(out) > 0 {
		return out, nil
	}
	return nil, fmt.Errorf("no IPv4 or IPv6 addresses resolved for %q", host)
}

func ListenDualStackTCP(ctx context.Context, addr string) (net.Listener, error) {
	if _, _, err := net.SplitHostPort(addr); err != nil {
		return nil, fmt.Errorf("invalid TCP bind address %q: %w", addr, err)
	}
	return (&net.ListenConfig{}).Listen(ctx, "tcp", addr)
}

func WrapH2CHandler(handler http.Handler, maxStreams uint32) http.Handler {
	if maxStreams == 0 {
		maxStreams = defaultMaxConcurrentStreams
	}
	return h2c.NewHandler(handler, &http2.Server{MaxConcurrentStreams: maxStreams})
}

func NewCleartextHTTPServer(addr string, handler http.Handler) *http.Server {
	return newBoundedHTTPServer(addr, WrapH2CHandler(handler, defaultMaxConcurrentStreams), nil)
}

func newBoundedHTTPServer(addr string, handler http.Handler, tlsCfg *tls.Config) *http.Server {
	return &http.Server{Addr: addr, Handler: handler, TLSConfig: tlsCfg, ReadHeaderTimeout: defaultReadHeaderTimeout, ReadTimeout: defaultRWTimeout, WriteTimeout: defaultRWTimeout, IdleTimeout: defaultIdleTimeout}
}

func NewMultiProtocolServer(ctx context.Context, cfg MultiProtocolConfig, handler http.Handler) (*MultiProtocolServer, error) {
	if handler == nil || (cfg.HTTPBindAddr == "" && cfg.HTTPSBindAddr == "") {
		return nil, errors.New("multiprotocol server requires handler and bind address")
	}
	srv := &MultiProtocolServer{altSvc: cfg.AltSvcHeader}
	if err := srv.initHTTPSAndQUIC(ctx, cfg); err != nil {
		return nil, err
	}
	wrapped := srv.wrapAltSvc(handler)
	if cfg.HTTPBindAddr != "" {
		ln, err := ListenDualStackTCP(ctx, cfg.HTTPBindAddr)
		if err != nil {
			return nil, errors.Join(err, srv.Shutdown(context.Background()))
		}
		srv.httpLn, srv.cleartextSrv = ln, newBoundedHTTPServer(ln.Addr().String(), WrapH2CHandler(wrapped, cfg.MaxConcurrentStreams), nil)
	}
	if srv.httpsLn != nil {
		srv.tlsSrv = newBoundedHTTPServer(srv.httpsLn.Addr().String(), wrapped, srv.tlsCfg)
		if err := http2.ConfigureServer(srv.tlsSrv, &http2.Server{MaxConcurrentStreams: max(cfg.MaxConcurrentStreams, defaultMaxConcurrentStreams)}); err != nil {
			return nil, errors.Join(err, srv.Shutdown(context.Background()))
		}
	}
	return srv, nil
}

func (s *MultiProtocolServer) initHTTPSAndQUIC(ctx context.Context, cfg MultiProtocolConfig) error {
	if cfg.HTTPSBindAddr == "" {
		return nil
	}
	tlsCfg, err := buildTLS13Config(cfg)
	if err != nil {
		return err
	}
	httpsLn, err := ListenDualStackTCP(ctx, cfg.HTTPSBindAddr)
	if err != nil {
		return err
	}
	s.tlsCfg, s.httpsLn = tlsCfg, httpsLn
	if cfg.DisableQUIC {
		return nil
	}
	quicAddr := cfg.QUICBindAddr
	if quicAddr == "" {
		quicAddr = httpsLn.Addr().String()
	}
	udpConn, err := net.ListenPacket("udp", quicAddr)
	if err != nil {
		host, _, _ := net.SplitHostPort(httpsLn.Addr().String())
		if udpConn, err = net.ListenPacket("udp", net.JoinHostPort(host, "0")); err != nil {
			return errors.Join(err, httpsLn.Close())
		}
	}
	s.quicConn = udpConn
	if s.altSvc == "" {
		if udpAddr, ok := udpConn.LocalAddr().(*net.UDPAddr); ok {
			s.altSvc = fmt.Sprintf(`h3=":%d"; ma=86400`, udpAddr.Port)
		}
	}
	return nil
}

func buildTLS13Config(cfg MultiProtocolConfig) (*tls.Config, error) {
	base := &tls.Config{}
	if cfg.TLSConfig != nil {
		base = cfg.TLSConfig.Clone()
	} else if cert, err := tls.LoadX509KeyPair(cfg.TLSCertFile, cfg.TLSKeyFile); err != nil {
		return nil, fmt.Errorf("load TLS keypair: %w", err)
	} else {
		base.Certificates = []tls.Certificate{cert}
	}
	base.MinVersion, base.NextProtos = tls.VersionTLS13, []string{"h3", "h2", "http/1.1"}
	return base, nil
}

func (s *MultiProtocolServer) wrapAltSvc(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.altSvc != "" && r.ProtoMajor < 3 {
			w.Header().Set("Alt-Svc", s.altSvc)
		}
		next.ServeHTTP(w, r)
	})
}

func (s *MultiProtocolServer) HTTPAddr() string  { return s.httpLn.Addr().String() }
func (s *MultiProtocolServer) HTTPSAddr() string { return s.httpsLn.Addr().String() }
func (s *MultiProtocolServer) QUICAddr() string  { return s.quicConn.LocalAddr().String() }
func (s *MultiProtocolServer) AltSvc() string    { return s.altSvc }
func (s *MultiProtocolServer) TLSConfig() *tls.Config {
	return s.tlsCfg.Clone()
}

func (s *MultiProtocolServer) Serve(ctx context.Context) error {
	g, gCtx := errgroup.WithContext(ctx)
	if s.cleartextSrv != nil && s.httpLn != nil {
		g.Go(func() error { return ignoreClosed(s.cleartextSrv.Serve(s.httpLn)) })
	}
	if s.tlsSrv != nil && s.httpsLn != nil {
		g.Go(func() error { return ignoreClosed(s.tlsSrv.Serve(tls.NewListener(s.httpsLn, s.tlsCfg))) })
	}
	if s.quicConn != nil {
		g.Go(func() error { return s.serveQUIC(gCtx) })
	}
	g.Go(func() error {
		<-gCtx.Done()
		shutCtx, cancel := context.WithTimeout(context.Background(), defaultShutdownTimeout)
		defer cancel()
		return s.Shutdown(shutCtx)
	})
	return g.Wait()
}

func ignoreClosed(err error) error {
	if errors.Is(err, http.ErrServerClosed) || errors.Is(err, net.ErrClosed) {
		return nil
	}
	return err
}

func (s *MultiProtocolServer) serveQUIC(ctx context.Context) error {
	buf := make([]byte, 1500)
	for ctx.Err() == nil {
		n, addr, err := s.quicConn.ReadFrom(buf)
		if err != nil {
			return nil
		}
		if n > 0 && addr != nil {
			_, _ = s.quicConn.WriteTo([]byte("h3:"+s.altSvc), addr)
		}
	}
	return nil
}

func (s *MultiProtocolServer) Shutdown(ctx context.Context) error {
	var err error
	s.closeOnce.Do(func() {
		var qErr, tErr, cErr error
		if s.quicConn != nil {
			qErr = ignoreClosed(s.quicConn.Close())
		}
		if s.tlsSrv != nil {
			tErr = ignoreClosed(s.tlsSrv.Shutdown(ctx))
		}
		if s.cleartextSrv != nil {
			cErr = ignoreClosed(s.cleartextSrv.Shutdown(ctx))
		}
		err = errors.Join(qErr, tErr, cErr)
	})
	return err
}
