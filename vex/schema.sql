-- Vex metadata schema.  Raw Git object and pack bytes never belong
-- here: they live in the blob store under git/v1/<repo_uuid>/objects/<oid>.

CREATE TABLE IF NOT EXISTS repositories (
	repo_id BINARY(16) NOT NULL,
	name VARBINARY(255) NULL,
	hash_algo VARCHAR(16) NOT NULL,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	deleted_at DATETIME(6) NULL,
	PRIMARY KEY (repo_id),
	UNIQUE KEY repositories_name (name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS generations (
	repo_id BINARY(16) NOT NULL,
	generation BIGINT UNSIGNED NOT NULL,
	state ENUM('open', 'sealed', 'sweepable', 'swept', 'aborted') NOT NULL,
	push_id BINARY(16) NULL,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	expires_at DATETIME(6) NULL,
	-- A live receive worker advances this lease; recovery falls back to
	-- created_at only for legacy rows which predate the heartbeat column.
	receive_heartbeat_at DATETIME(6) NULL,
	sealed_at DATETIME(6) NULL,
	PRIMARY KEY (repo_id, generation),
	KEY generations_open_heartbeat (repo_id, state, receive_heartbeat_at),
	CONSTRAINT generations_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS objects (
	repo_id BINARY(16) NOT NULL,
	oid BINARY(32) NOT NULL,
	obj_type TINYINT UNSIGNED NOT NULL,
	size BIGINT UNSIGNED NOT NULL,
	-- This records a key, never object payload bytes.
	blob_key VARBINARY(512) NOT NULL,
	generation BIGINT UNSIGNED NOT NULL,
	state ENUM('quarantine', 'published') NOT NULL,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	published_at DATETIME(6) NULL,
	PRIMARY KEY (repo_id, oid),
	KEY objects_generation (repo_id, generation, state),
	CONSTRAINT objects_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id),
	CONSTRAINT objects_generation_fk FOREIGN KEY (repo_id, generation)
		REFERENCES generations (repo_id, generation)
) ENGINE=InnoDB;

-- A deduplicated object can be staged by more than one open epoch.  This
-- relation lets compensation remove one epoch without deleting another's
-- visibility claim; it remains metadata only.
CREATE TABLE IF NOT EXISTS generation_objects (
	repo_id BINARY(16) NOT NULL,
	generation BIGINT UNSIGNED NOT NULL,
	oid BINARY(32) NOT NULL,
	PRIMARY KEY (repo_id, generation, oid),
	KEY generation_objects_oid (repo_id, oid),
	CONSTRAINT generation_objects_generation_fk FOREIGN KEY (repo_id, generation)
		REFERENCES generations (repo_id, generation),
	CONSTRAINT generation_objects_object_fk FOREIGN KEY (repo_id, oid)
		REFERENCES objects (repo_id, oid)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS commit_parents (
	repo_id BINARY(16) NOT NULL,
	commit_oid BINARY(32) NOT NULL,
	parent_pos INT UNSIGNED NOT NULL,
	parent_oid BINARY(32) NOT NULL,
	PRIMARY KEY (repo_id, commit_oid, parent_pos),
	KEY commit_parents_parent (repo_id, parent_oid),
	CONSTRAINT commit_parents_object_fk FOREIGN KEY (repo_id, commit_oid)
		REFERENCES objects (repo_id, oid)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS refs (
	repo_id BINARY(16) NOT NULL,
	refname VARBINARY(1024) NOT NULL,
	is_symbolic BOOLEAN NOT NULL DEFAULT FALSE,
	target_oid BINARY(32) NULL,
	symref_target VARBINARY(1024) NULL,
	peeled_oid BINARY(32) NULL,
	version BIGINT UNSIGNED NOT NULL,
	updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
		ON UPDATE CURRENT_TIMESTAMP(6),
	PRIMARY KEY (repo_id, refname),
	CONSTRAINT refs_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS ref_updates (
	repo_id BINARY(16) NOT NULL,
	-- IDs are ordered within a repository.  Keep a non-unique leading index so
	-- direct SQL callers may still use AUTO_INCREMENT without serializing all
	-- repositories behind one global audit-id sequence.
	id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	refname VARBINARY(1024) NOT NULL,
	old_oid BINARY(32) NULL,
	new_oid BINARY(32) NULL,
	old_target VARBINARY(1024) NULL,
	new_target VARBINARY(1024) NULL,
	is_ff BOOLEAN NULL,
	actor VARBINARY(255) NULL,
	push_id BINARY(16) NULL,
	reason VARCHAR(32) NOT NULL,
	message VARBINARY(1024) NULL,
	ts DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	PRIMARY KEY (repo_id, id),
	KEY ref_updates_id (id),
	KEY ref_updates_ref (repo_id, refname, id),
	CONSTRAINT ref_updates_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS object_pins (
	repo_id BINARY(16) NOT NULL,
	oid BINARY(32) NOT NULL,
	reason VARBINARY(255) NOT NULL,
	expires_at DATETIME(6) NULL,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	PRIMARY KEY (repo_id, oid, reason),
	KEY object_pins_expiry (repo_id, expires_at),
	CONSTRAINT object_pins_object_fk FOREIGN KEY (repo_id, oid)
		REFERENCES objects (repo_id, oid)
) ENGINE=InnoDB;

-- Immutable pack metadata.  Derived packs are an optional read cache; a
-- `receive` envelope is a canonical S3 pack/idx pair for the narrow
-- full-mirror seed path.  In either case, MySQL stores only keys and object
-- metadata -- never a pack, index, or object payload.
CREATE TABLE IF NOT EXISTS pack_envelopes (
	repo_id BINARY(16) NOT NULL,
	pack_id BINARY(32) NOT NULL,
	pack_key VARBINARY(512) NOT NULL,
	idx_key VARBINARY(512) NULL,
	bitmap_key VARBINARY(512) NULL,
	covering_generation BIGINT UNSIGNED NOT NULL,
	state ENUM('building', 'sealed', 'obsolete', 'deleted') NOT NULL,
	source_kind ENUM('derived', 'receive') NOT NULL DEFAULT 'derived',
	-- Set only for a raw receive envelope, so recovery can find its epoch
	-- without treating `covering_generation` as a derived-cache contract.
	receive_generation BIGINT UNSIGNED NULL,
	-- A raw incremental pack must never be selected as a complete clone seed.
	is_full_snapshot BOOLEAN NOT NULL DEFAULT TRUE,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	sealed_at DATETIME(6) NULL,
	obsolete_at DATETIME(6) NULL,
	PRIMARY KEY (repo_id, pack_id),
	KEY pack_envelopes_covering (repo_id, state, covering_generation),
	KEY pack_envelopes_receive (repo_id, source_kind, state, receive_generation),
	CONSTRAINT pack_envelopes_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS pack_objects (
	repo_id BINARY(16) NOT NULL,
	pack_id BINARY(32) NOT NULL,
	oid BINARY(32) NOT NULL,
	PRIMARY KEY (repo_id, pack_id, oid),
	KEY pack_objects_oid (repo_id, oid),
	CONSTRAINT pack_objects_pack_fk FOREIGN KEY (repo_id, pack_id)
		REFERENCES pack_envelopes (repo_id, pack_id),
	CONSTRAINT pack_objects_object_fk FOREIGN KEY (repo_id, oid)
		REFERENCES objects (repo_id, oid)
) ENGINE=InnoDB;

-- The receive pack's object map is intentionally separate from pack_objects:
-- the latter has an FK to objects and therefore represents only derived
-- cache packs.  This table lets one bulk metadata import record a raw pack's
-- membership without writing 847k loose-object records.  Envelope state is
-- the visibility bit for every member; offsets remain authoritative in .idx.
CREATE TABLE IF NOT EXISTS received_pack_objects (
	repo_id BINARY(16) NOT NULL,
	pack_id BINARY(32) NOT NULL,
	oid BINARY(32) NOT NULL,
	obj_type TINYINT UNSIGNED NOT NULL,
	size BIGINT UNSIGNED NOT NULL,
	PRIMARY KEY (repo_id, pack_id, oid),
	KEY received_pack_objects_lookup (repo_id, oid, pack_id),
	CONSTRAINT received_pack_objects_pack_fk FOREIGN KEY (repo_id, pack_id)
		REFERENCES pack_envelopes (repo_id, pack_id)
) ENGINE=InnoDB;

-- A live upload-pack/cache reader leases one immutable pack while it may
-- serve its members. There is intentionally no foreign key to pack_envelopes:
-- the state adapter rewrites immutable pack rows inside its repository
-- transaction, while a lease must survive that short rewrite and expire by
-- its TTL if a server process dies.
CREATE TABLE IF NOT EXISTS clone_leases (
	repo_id BINARY(16) NOT NULL,
	lease_id BINARY(16) NOT NULL,
	pack_id BINARY(32) NOT NULL,
	expires_at DATETIME(6) NOT NULL,
	created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	PRIMARY KEY (repo_id, lease_id),
	KEY clone_leases_active (repo_id, expires_at, pack_id),
	CONSTRAINT clone_leases_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

-- A compact, durable work signal for the asynchronous derived-pack builder.
-- A receive only advances target_generation; it never builds a pack while
-- holding the repository publish boundary.  not_before starts one bounded
-- debounce window for a batch and is intentionally not pushed forward by
-- every later receive, so a busy repository cannot starve its builder.  The
-- worker lease is metadata only: Git object and pack bytes remain in blob
-- storage, and a claimed target keeps a slow worker from publishing stale
-- work after a newer receive advances the request.
CREATE TABLE IF NOT EXISTS pack_rebuild_requests (
	repo_id BINARY(16) NOT NULL,
	target_generation BIGINT UNSIGNED NOT NULL,
	requested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	last_requested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	not_before DATETIME(6) NOT NULL,
	worker_lease_id BINARY(16) NULL,
	claimed_target_generation BIGINT UNSIGNED NULL,
	worker_lease_expires_at DATETIME(6) NULL,
	worker_claimed_at DATETIME(6) NULL,
	attempts INT UNSIGNED NOT NULL DEFAULT 0,
	last_error VARBINARY(1024) NULL,
	PRIMARY KEY (repo_id),
	KEY pack_rebuild_requests_due (not_before, repo_id),
	KEY pack_rebuild_requests_claimable
		(not_before, worker_lease_expires_at, repo_id),
	CONSTRAINT pack_rebuild_requests_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;

-- A durable, metadata-only record of the destructive sweep protocol.  An
-- object remains readable while metadata_deleted_at is NULL.  Once metadata
-- is removed, the row makes an interrupted S3 delete retryable without ever
-- restoring a dangling metadata pointer to missing canonical bytes.
CREATE TABLE IF NOT EXISTS object_sweep_tombstones (
	repo_id BINARY(16) NOT NULL,
	oid BINARY(32) NOT NULL,
	generation BIGINT UNSIGNED NOT NULL,
	marked_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
	delete_after DATETIME(6) NOT NULL,
	metadata_deleted_at DATETIME(6) NULL,
	blob_deleted_at DATETIME(6) NULL,
	PRIMARY KEY (repo_id, oid),
	KEY object_sweep_tombstones_pending (repo_id, metadata_deleted_at, blob_deleted_at, delete_after),
	CONSTRAINT object_sweep_tombstones_repo_fk FOREIGN KEY (repo_id)
		REFERENCES repositories (repo_id)
) ENGINE=InnoDB;
