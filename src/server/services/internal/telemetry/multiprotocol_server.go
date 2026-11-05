// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package telemetry

import (
	"bytes"
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/quic-go/qpack"
	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"golang.org/x/net/http2/hpack"
	"golang.org/x/sync/errgroup"
)

const (
	defaultMaxConcurrentStreams = 250
	defaultMaxDynamicTableSize  = 4096
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
	h3Srv                *http3.Server
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
	return newCleartextBoundedServer(addr, handler, defaultMaxConcurrentStreams)
}

func newCleartextBoundedServer(addr string, handler http.Handler, maxStreams uint32) *http.Server {
	if maxStreams == 0 {
		maxStreams = defaultMaxConcurrentStreams
	}
	var protos http.Protocols
	protos.SetHTTP1(true)
	protos.SetUnencryptedHTTP2(true)
	srv := newBoundedHTTPServer(addr, WrapH2CHandler(handler, maxStreams), nil)
	srv.Protocols = &protos
	srv.HTTP2 = &http.HTTP2Config{MaxConcurrentStreams: int(maxStreams)}
	return srv
}

func newBoundedHTTPServer(addr string, handler http.Handler, tlsCfg *tls.Config) *http.Server {
	return &http.Server{Addr: addr, Handler: handler, TLSConfig: tlsCfg, ReadHeaderTimeout: defaultReadHeaderTimeout, ReadTimeout: defaultRWTimeout, WriteTimeout: defaultRWTimeout, IdleTimeout: defaultIdleTimeout}
}

func NewH2CClient(timeout time.Duration) *http.Client {
	if timeout <= 0 {
		timeout = defaultRWTimeout
	}
	dialer := &net.Dialer{Timeout: timeout, KeepAlive: 30 * time.Second}
	return &http.Client{
		Timeout: timeout,
		Transport: &http2.Transport{
			AllowHTTP: true,
			DialTLSContext: func(ctx context.Context, network, addr string, _ *tls.Config) (net.Conn, error) {
				return dialer.DialContext(ctx, network, addr)
			},
			MaxHeaderListSize: 1 << 16,
			ReadIdleTimeout:   30 * time.Second,
			PingTimeout:       5 * time.Second,
		},
	}
}

func NewH3Client(tlsCfg *tls.Config, timeout time.Duration) *http.Client {
	if timeout <= 0 {
		timeout = defaultRWTimeout
	}
	return &http.Client{
		Timeout: timeout,
		Transport: &http3.Transport{
			TLSClientConfig: buildClientH3TLS(tlsCfg),
			QUICConfig:      &quic.Config{MaxIdleTimeout: defaultIdleTimeout},
		},
	}
}

func H3RoundTrip(ctx context.Context, tlsCfg *tls.Config, req *http.Request) (*http.Response, error) {
	tr := &http3.Transport{
		TLSClientConfig: buildClientH3TLS(tlsCfg),
		QUICConfig:      &quic.Config{MaxIdleTimeout: defaultIdleTimeout},
	}
	defer func() { _ = tr.Close() }()
	resp, err := tr.RoundTrip(req.WithContext(ctx))
	if err != nil {
		return nil, err
	}
	if resp.Body != nil {
		bodyBytes, readErr := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		if readErr != nil {
			return nil, readErr
		}
		resp.Body = io.NopCloser(bytes.NewReader(bodyBytes))
		resp.ContentLength = int64(len(bodyBytes))
	}
	return resp, nil
}

type multiProtocolTransport struct {
	h2c  *http2.Transport
	h2   *http.Transport
	tls  *tls.Config
	mu   sync.RWMutex
	h3At map[string]string
}

func NewMultiProtocolClient(tlsCfg *tls.Config, timeout time.Duration) *http.Client {
	if timeout <= 0 {
		timeout = defaultRWTimeout
	}
	h2TLS := buildClientH2TLS(tlsCfg)
	return &http.Client{
		Timeout: timeout,
		Transport: &multiProtocolTransport{
			h2c:  NewH2CClient(timeout).Transport.(*http2.Transport),
			h2:   &http.Transport{TLSClientConfig: h2TLS, ForceAttemptHTTP2: true, IdleConnTimeout: defaultIdleTimeout},
			tls:  buildClientH3TLS(tlsCfg),
			h3At: make(map[string]string),
		},
	}
}

func (m *multiProtocolTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if req.URL.Scheme == "http" {
		return m.h2c.RoundTrip(req)
	}
	if req.ProtoMajor == 3 {
		return H3RoundTrip(req.Context(), m.tls, req)
	}
	if h3Addr := m.lookupH3Addr(req.URL.Host); h3Addr != "" {
		h3Req := req.Clone(req.Context())
		h3Req.URL.Host = h3Addr
		if resp, err := H3RoundTrip(req.Context(), m.tls, h3Req); err == nil {
			return resp, nil
		}
	}
	resp, err := m.h2.RoundTrip(req)
	if err == nil && resp != nil {
		m.recordAltSvc(req.URL.Host, resp.Header.Get("Alt-Svc"))
	}
	return resp, err
}

func (m *multiProtocolTransport) lookupH3Addr(host string) string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.h3At[host]
}

func (m *multiProtocolTransport) recordAltSvc(originHost, altSvc string) {
	port := parseAltSvcH3Port(altSvc)
	if port == "" {
		return
	}
	host := originHost
	if h, _, err := net.SplitHostPort(originHost); err == nil {
		host = h
	}
	m.mu.Lock()
	m.h3At[originHost] = net.JoinHostPort(host, port)
	m.mu.Unlock()
}

func parseAltSvcH3Port(altSvc string) string {
	idx := strings.Index(altSvc, `h3=":`)
	if idx < 0 {
		return ""
	}
	rest := altSvc[idx+len(`h3=":`):]
	end := strings.IndexByte(rest, '"')
	if end <= 0 {
		return ""
	}
	return rest[:end]
}

func buildClientH2TLS(base *tls.Config) *tls.Config {
	cfg := &tls.Config{MinVersion: tls.VersionTLS13}
	if base != nil {
		cfg = base.Clone()
		cfg.MinVersion = tls.VersionTLS13
	}
	cfg.NextProtos = []string{"h2", "http/1.1"}
	return cfg
}

func buildClientH3TLS(base *tls.Config) *tls.Config {
	cfg := &tls.Config{MinVersion: tls.VersionTLS13}
	if base != nil {
		cfg = base.Clone()
		cfg.MinVersion = tls.VersionTLS13
	}
	cfg.NextProtos = []string{http3.NextProtoH3}
	return cfg
}

func EncodeHPACKHeaders(hdr http.Header) ([]byte, error) {
	var buf bytes.Buffer
	enc := hpack.NewEncoder(&buf)
	enc.SetMaxDynamicTableSizeLimit(defaultMaxDynamicTableSize)
	for _, k := range sortedHeaderKeys(hdr) {
		name := strings.ToLower(k)
		for _, v := range hdr[k] {
			if err := enc.WriteField(hpack.HeaderField{Name: name, Value: v}); err != nil {
				return nil, fmt.Errorf("hpack encode %q: %w", name, err)
			}
		}
	}
	return buf.Bytes(), nil
}

func DecodeHPACKHeaders(block []byte) (http.Header, error) {
	out := make(http.Header)
	dec := hpack.NewDecoder(defaultMaxDynamicTableSize, func(f hpack.HeaderField) {
		out.Add(f.Name, f.Value)
	})
	if _, err := dec.Write(block); err != nil {
		return nil, fmt.Errorf("hpack decode: %w", err)
	}
	if err := dec.Close(); err != nil {
		return nil, fmt.Errorf("hpack close: %w", err)
	}
	return out, nil
}

func EncodeQPACKHeaders(hdr http.Header) ([]byte, error) {
	var buf bytes.Buffer
	enc := qpack.NewEncoder(&buf)
	for _, k := range sortedHeaderKeys(hdr) {
		name := strings.ToLower(k)
		for _, v := range hdr[k] {
			if err := enc.WriteField(qpack.HeaderField{Name: name, Value: v}); err != nil {
				return nil, fmt.Errorf("qpack encode %q: %w", name, err)
			}
		}
	}
	if err := enc.Close(); err != nil {
		return nil, fmt.Errorf("qpack close: %w", err)
	}
	return buf.Bytes(), nil
}

func DecodeQPACKHeaders(block []byte) (http.Header, error) {
	out := make(http.Header)
	decode := qpack.NewDecoder().Decode(block)
	for {
		hf, err := decode()
		if errors.Is(err, io.EOF) {
			return out, nil
		}
		if err != nil {
			return nil, fmt.Errorf("qpack decode: %w", err)
		}
		out.Add(hf.Name, hf.Value)
	}
}

func sortedHeaderKeys(hdr http.Header) []string {
	keys := make([]string, 0, len(hdr))
	for k := range hdr {
		keys = append(keys, k)
	}
	slices.Sort(keys)
	return keys
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
		srv.httpLn, srv.cleartextSrv = ln, newCleartextBoundedServer(ln.Addr().String(), wrapped, cfg.MaxConcurrentStreams)
	}
	if srv.httpsLn != nil {
		srv.tlsSrv = newBoundedHTTPServer(srv.httpsLn.Addr().String(), wrapped, srv.tlsCfg)
		if err := http2.ConfigureServer(srv.tlsSrv, &http2.Server{MaxConcurrentStreams: max(cfg.MaxConcurrentStreams, defaultMaxConcurrentStreams)}); err != nil {
			return nil, errors.Join(err, srv.Shutdown(context.Background()))
		}
	}
	if srv.quicConn != nil {
		srv.h3Srv = &http3.Server{
			Handler:     wrapped,
			TLSConfig:   http3.ConfigureTLSConfig(srv.tlsCfg),
			QUICConfig:  &quic.Config{MaxIdleTimeout: defaultIdleTimeout},
			IdleTimeout: defaultIdleTimeout,
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
		if s.altSvc == "" {
			s.altSvc = "clear"
		}
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
	base.MinVersion, base.NextProtos = tls.VersionTLS13, []string{http3.NextProtoH3, "h2", "http/1.1"}
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
	if s.h3Srv != nil && s.quicConn != nil {
		g.Go(func() error { return ignoreClosed(s.h3Srv.Serve(s.quicConn)) })
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
	if errors.Is(err, http.ErrServerClosed) || errors.Is(err, net.ErrClosed) || errors.Is(err, quic.ErrServerClosed) {
		return nil
	}
	return err
}

func (s *MultiProtocolServer) Shutdown(ctx context.Context) error {
	var err error
	s.closeOnce.Do(func() {
		var h3Err, qErr, tErr, cErr error
		if s.h3Srv != nil {
			h3Err = ignoreClosed(s.h3Srv.Close())
		}
		if s.quicConn != nil {
			qErr = ignoreClosed(s.quicConn.Close())
		}
		if s.tlsSrv != nil {
			tErr = ignoreClosed(s.tlsSrv.Shutdown(ctx))
		}
		if s.cleartextSrv != nil {
			cErr = ignoreClosed(s.cleartextSrv.Shutdown(ctx))
		}
		err = errors.Join(h3Err, qErr, tErr, cErr)
	})
	return err
}
