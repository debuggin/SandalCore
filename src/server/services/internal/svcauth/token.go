// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package svcauth

import (
	"fmt"
	"os"
	"strings"
)

// ReadTokenBytes reads a service token from path, falling back to the
// AOC_SERVICE_TOKEN (or SANDAL_SERVICE_TOKEN) environment variable when the
// file does not exist on disk (e.g. inside a read-only distroless container).
func ReadTokenBytes(path string) ([]byte, error) {
	if path != "" {
		if b, err := os.ReadFile(path); err == nil {
			return []byte(strings.TrimSpace(string(b))), nil
		} else if tok := envToken(); tok != "" {
			return []byte(tok), nil
		} else {
			return nil, fmt.Errorf("service token: %w (scripts\\services.ps1 run creates it)", err)
		}
	}
	if tok := envToken(); tok != "" {
		return []byte(tok), nil
	}
	return nil, fmt.Errorf("service token: empty path and AOC_SERVICE_TOKEN not set")
}

// LoadOrEnv loads the service token from path, falling back to the
// AOC_SERVICE_TOKEN environment variable when the token file is absent.
func LoadOrEnv(path string) (Token, error) {
	t, err := Load(path)
	if err == nil {
		return t, nil
	}
	if tok := envToken(); tok != "" {
		return Token(tok), nil
	}
	return "", err
}

func envToken() string {
	if tok := strings.TrimSpace(os.Getenv("AOC_SERVICE_TOKEN")); tok != "" {
		return tok
	}
	return strings.TrimSpace(os.Getenv("SANDAL_SERVICE_TOKEN"))
}
