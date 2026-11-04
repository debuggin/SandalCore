// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
	"os"
	"strconv"

	_ "modernc.org/sqlite" // the pure-Go SQLite driver, registered as "sqlite"
)

// DB is an opened content.db.
type DB struct {
	sql      *sql.DB
	encoding string // how record.data is stored (meta record.encoding)
}

// Open opens the content.db at path for reading and checks its schema version.
// A path that does not exist is an error and is not created: SQLite would make
// an empty file of it, which a second run would then open as a database with no
// meta table.
func Open(path string) (*DB, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("content: %w", err)
	}
	db, err := sql.Open("sqlite", path+"?_pragma=query_only(1)")
	if err != nil {
		return nil, err
	}
	d := &DB{sql: db}
	meta, err := d.Meta()
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("content: %s: %w", path, err)
	}
	if got := meta[MetaSchemaVersion]; got != strconv.Itoa(SchemaVersion) {
		db.Close()
		return nil, fmt.Errorf("content: %s has schema version %q, this reader wants %d", path, got, SchemaVersion)
	}
	d.encoding = meta[MetaRecordEncoding]
	return d, nil
}

// built by C.O.A with Cyb

// Close closes the database.
func (d *DB) Close() error { return d.sql.Close() }

// SQL is the underlying handle, for queries the typed ones do not cover.
func (d *DB) SQL() *sql.DB { return d.sql }

// Meta returns the meta table.
func (d *DB) Meta() (map[string]string, error) {
	rows, err := d.sql.Query(`SELECT key, value FROM meta`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]string{}
	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			return nil, err
		}
		out[k] = v
	}
	return out, rows.Err()
}
