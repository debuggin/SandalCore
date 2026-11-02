// SPDX-License-Identifier: AGPL-3.0-only
package config

import "strings"

func RedactDSN(dsn string) string {
	if i := strings.Index(dsn, "://"); i >= 0 {
		rest := dsn[i+3:]
		if at, sl := strings.IndexByte(rest, '@'), strings.IndexAny(rest, "/?"); at > 0 && (sl < 0 || at < sl) {
			if c := strings.IndexByte(rest[:at], ':'); c >= 0 && c+1 < at {
				dsn = dsn[:i+3+c+1] + "***REDACTED***" + rest[at:]
			}
		}
	}
	if idx := strings.Index(dsn, "password="); idx >= 0 {
		rest := dsn[idx+9:]
		end := strings.IndexAny(rest, " \t&;")
		if end < 0 {
			end = len(rest)
		}
		if end > 0 {
			dsn = dsn[:idx+9] + "***REDACTED***" + rest[end:]
		}
	}
	return dsn
}
