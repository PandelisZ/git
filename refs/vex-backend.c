#include "../git-compat-util.h"
#include "../gettext.h"
#include "../hex.h"
#include "../object.h"
#include "../odb/source-vex.h"
#include "../odb/transaction.h"
#include "../refs.h"
#include "../repository.h"
#include "../strbuf.h"
#include "../vex/store.h"
#include "refs-internal.h"

/*
 * Vex is the source of truth for both object metadata and references.  The
 * gitdir still contains the normal discovery stubs, but this backend never
 * reads or writes loose refs below it.
 */
struct vex_ref_store {
	struct ref_store base;
	struct vex_store *store;
	unsigned int store_flags;
};

struct vex_transaction_data {
	struct vex_epoch *epoch;
	struct vex_ref_transaction *transaction;
	/* Non-NULL only while receive-pack's ODB epoch is being handed off. */
	struct odb_transaction *receive_transaction;
};

struct vex_ref_iterator_entry {
	char *name;
	struct vex_ref_target target;
};

struct vex_ref_iterator {
	struct ref_iterator base;
	struct vex_ref_store *refs;
	struct vex_ref_iterator_entry *entries;
	size_t nr, alloc, pos;
	size_t exclude_alloc;
	char *prefix;
	char **exclude_patterns;
	unsigned int flags;
	struct object_id oid;
};

static struct vex_ref_store *vex_downcast(struct ref_store *ref_store)
{
	if (ref_store->be != &refs_be_vex)
		BUG("ref store is type \"%s\", not \"vex\"", ref_store->be->name);
	return (struct vex_ref_store *)ref_store;
}

static void vex_target_from_ref(struct vex_ref_target *target,
				const struct vex_ref *ref, int exists)
{
	memset(target, 0, sizeof(*target));
	if (!exists) {
		target->type = VEX_REF_TARGET_NONE;
		return;
	}
	*target = ref->target;
}

static int vex_read_current_ref(struct vex_ref_store *refs, const char *refname,
				struct vex_ref *ref, int *exists)
{
	int ret;

	memset(ref, 0, sizeof(*ref));
	*exists = 0;
	ret = vex_ref_read(refs->store, refname, ref);
	if (ret == VEX_STORE_ERR_NOT_FOUND)
		return 0;
	if (ret)
		return ret;
	*exists = 1;
	return 0;
}

static int vex_be_create_on_disk(struct ref_store *ref_store,
				 int flags UNUSED, struct strbuf *err)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	int ret = vex_repo_ensure(refs->store);

	if (ret)
		strbuf_addstr(err, _("unable to initialize Vex reference store"));
	return ret;
}

static int vex_be_remove_on_disk(struct ref_store *ref_store UNUSED,
				 struct strbuf *err)
{
	strbuf_addstr(err, _("removing the Vex reference store is not supported"));
	return -1;
}

static struct ref_store *vex_be_init(struct repository *repo,
				     const char *payload UNUSED,
				     const char *gitdir,
				     const struct ref_store_init_options *opts)
{
	struct vex_ref_store *refs = xcalloc(1, sizeof(*refs));

	base_ref_store_init(&refs->base, repo, gitdir, &refs_be_vex);
	refs->store_flags = opts->access_flags;
	if (vex_store_open_for_repository(repo, gitdir, &refs->store))
		die(_("unable to open Vex reference store for '%s'"),
		    repo->ref_storage_payload ? repo->ref_storage_payload : "");
	return &refs->base;
}

static void vex_be_release(struct ref_store *ref_store)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);

	if (refs->store) {
		vex_store_close(refs->store);
		refs->store = NULL;
	}
}

static int vex_be_read_raw_ref(struct ref_store *ref_store, const char *refname,
				       struct object_id *oid, struct strbuf *referent,
				       unsigned int *type, int *failure_errno)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	struct vex_ref ref = {0};
	int ret;

	*type = 0;
	ret = vex_ref_read(refs->store, refname, &ref);
	if (ret == VEX_STORE_ERR_NOT_FOUND) {
		*failure_errno = ENOENT;
		return -1;
	}
	if (ret) {
		*failure_errno = EIO;
		return -1;
	}

	switch (ref.target.type) {
	case VEX_REF_TARGET_OID:
		oidcpy(oid, &ref.target.oid);
		break;
	case VEX_REF_TARGET_SYMREF:
		strbuf_reset(referent);
		strbuf_addstr(referent, ref.target.symref);
		*type |= REF_ISSYMREF;
		break;
	default:
		*failure_errno = EIO;
		ret = -1;
		break;
	}
	vex_ref_clear(&ref);
	return ret;
}

static int vex_be_read_symbolic_ref(struct ref_store *ref_store,
				    const char *refname, struct strbuf *referent)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	struct vex_ref ref = {0};
	int ret;

	ret = vex_ref_read(refs->store, refname, &ref);
	if (ret)
		return -1;
	if (ref.target.type != VEX_REF_TARGET_SYMREF) {
		vex_ref_clear(&ref);
		return NOT_A_SYMREF;
	}
	strbuf_reset(referent);
	strbuf_addstr(referent, ref.target.symref);
	vex_ref_clear(&ref);
	return 0;
}

static int vex_ref_iterator_collect(const struct vex_ref *ref, void *cb_data)
{
	struct vex_ref_iterator *iter = cb_data;
	struct vex_ref_iterator_entry *entry;

	if (ref->target.type != VEX_REF_TARGET_OID &&
	    ref->target.type != VEX_REF_TARGET_SYMREF)
		return 0;

	ALLOC_GROW(iter->entries, iter->nr + 1, iter->alloc);
	entry = &iter->entries[iter->nr++];
	entry->name = xstrdup(ref->name);
	entry->target = ref->target;
	if (entry->target.type == VEX_REF_TARGET_SYMREF)
		entry->target.symref = xstrdup(ref->target.symref);
	return 0;
}

static int vex_ref_iterator_entry_cmp(const void *va, const void *vb)
{
	const struct vex_ref_iterator_entry *a = va;
	const struct vex_ref_iterator_entry *b = vb;

	return strcmp(a->name, b->name);
}

static int vex_ref_iterator_excludes(struct vex_ref_iterator *iter,
				     const char *refname)
{
	if (!iter->exclude_patterns)
		return 0;
	for (size_t i = 0; iter->exclude_patterns[i]; i++)
		if (starts_with(refname, iter->exclude_patterns[i]))
			return 1;
	return 0;
}

static int vex_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct vex_ref_iterator *iter = (struct vex_ref_iterator *)ref_iterator;
	struct vex_ref_iterator_entry *entry;
	const char *target = NULL;
	int flags;

	while (iter->pos < iter->nr) {
		entry = &iter->entries[iter->pos++];
		if (iter->prefix && !starts_with(entry->name, iter->prefix))
			continue;
		if (!starts_with(entry->name, "refs/") &&
		    !(iter->flags & REFS_FOR_EACH_INCLUDE_ROOT_REFS &&
		      is_root_ref(entry->name)))
			continue;
		if (vex_ref_iterator_excludes(iter, entry->name))
			continue;
		if (iter->flags & REFS_FOR_EACH_PER_WORKTREE_ONLY &&
		    parse_worktree_ref(entry->name, NULL, NULL, NULL) !=
			    REF_WORKTREE_CURRENT)
			continue;

		flags = 0;
		if (entry->target.type == VEX_REF_TARGET_OID) {
			oidcpy(&iter->oid, &entry->target.oid);
		} else {
			target = entry->target.symref;
			if (!refs_resolve_ref_unsafe(&iter->refs->base, entry->name,
						 RESOLVE_REF_READING, &iter->oid, &flags))
				oidclr(&iter->oid, iter->refs->base.repo->hash_algo);
		}

		if (is_null_oid(&iter->oid))
			flags |= REF_ISBROKEN;
		if (check_refname_format(entry->name, REFNAME_ALLOW_ONELEVEL)) {
			if (!refname_is_safe(entry->name))
				die(_("refname is dangerous: %s"), entry->name);
			oidclr(&iter->oid, iter->refs->base.repo->hash_algo);
			flags |= REF_BAD_NAME | REF_ISBROKEN;
		}
		if (iter->flags & REFS_FOR_EACH_OMIT_DANGLING_SYMREFS &&
		    flags & REF_ISSYMREF && flags & REF_ISBROKEN)
			continue;
		if (!(iter->flags & REFS_FOR_EACH_INCLUDE_BROKEN) &&
		    !ref_resolves_to_object(entry->name, iter->refs->base.repo,
					    &iter->oid, flags))
			continue;

		memset(&iter->base.ref, 0, sizeof(iter->base.ref));
		iter->base.ref.name = entry->name;
		iter->base.ref.target = target;
		iter->base.ref.oid = &iter->oid;
		iter->base.ref.flags = flags;
		return ITER_OK;
	}

	return ITER_DONE;
}

static int vex_ref_iterator_seek(struct ref_iterator *ref_iterator,
				 const char *refname, unsigned int flags)
{
	struct vex_ref_iterator *iter = (struct vex_ref_iterator *)ref_iterator;

	FREE_AND_NULL(iter->prefix);
	if (flags & REF_ITERATOR_SEEK_SET_PREFIX)
		iter->prefix = xstrdup_or_null(refname);
	iter->pos = 0;
	return 0;
}

static void vex_ref_iterator_release(struct ref_iterator *ref_iterator)
{
	struct vex_ref_iterator *iter = (struct vex_ref_iterator *)ref_iterator;

	for (size_t i = 0; i < iter->nr; i++) {
		free(iter->entries[i].name);
		if (iter->entries[i].target.type == VEX_REF_TARGET_SYMREF)
			free((char *)iter->entries[i].target.symref);
	}
	free(iter->entries);
	if (iter->exclude_patterns) {
		for (size_t i = 0; iter->exclude_patterns[i]; i++)
			free(iter->exclude_patterns[i]);
		free(iter->exclude_patterns);
	}
	free(iter->prefix);
}

static struct ref_iterator_vtable vex_ref_iterator_vtable = {
	.advance = vex_ref_iterator_advance,
	.seek = vex_ref_iterator_seek,
	.release = vex_ref_iterator_release,
};

static struct ref_iterator *vex_be_iterator_begin(struct ref_store *ref_store,
					   const char *prefix,
					   const char **exclude_patterns,
					   unsigned int flags)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	struct vex_ref_iterator *iter = xcalloc(1, sizeof(*iter));
	int ret;

	base_ref_iterator_init(&iter->base, &vex_ref_iterator_vtable);
	iter->refs = refs;
	iter->flags = flags;
	iter->prefix = xstrdup_or_null(prefix);
	if (exclude_patterns) {
		size_t i;
		for (i = 0; exclude_patterns[i]; i++) {
			ALLOC_GROW(iter->exclude_patterns, i + 2, iter->exclude_alloc);
			iter->exclude_patterns[i] = xstrdup(exclude_patterns[i]);
			iter->exclude_patterns[i + 1] = NULL;
		}
	}
	ret = vex_ref_iterate(refs->store, NULL, vex_ref_iterator_collect, iter);
	if (ret) {
		ref_iterator_free(&iter->base);
		return empty_ref_iterator_begin();
	}
	QSORT(iter->entries, iter->nr, vex_ref_iterator_entry_cmp);
	return &iter->base;
}

static enum ref_transaction_error vex_validate_old_ref(struct ref_update *update,
						const struct vex_ref *current,
						int exists, struct strbuf *err)
{
	if (update->flags & REF_LOG_ONLY)
		return 0;
	if (update->old_target) {
		if (!exists)
			return ref_update_check_old_target("", update, err);
		if (current->target.type != VEX_REF_TARGET_SYMREF) {
			strbuf_addf(err, _("cannot lock ref '%s': expected symref with target '%s': but is a regular ref"),
				    ref_update_original_update_refname(update), update->old_target);
			return REF_TRANSACTION_ERROR_EXPECTED_SYMREF;
		}
		return ref_update_check_old_target(current->target.symref, update, err);
	}
	if (!(update->flags & REF_HAVE_OLD))
		return 0;
	if (!exists) {
		if (is_null_oid(&update->old_oid))
			return 0;
		strbuf_addf(err, _("cannot lock ref '%s': reference is missing but expected %s"),
			    ref_update_original_update_refname(update), oid_to_hex(&update->old_oid));
		return REF_TRANSACTION_ERROR_NONEXISTENT_REF;
	}
	if (current->target.type == VEX_REF_TARGET_OID &&
	    oideq(&current->target.oid, &update->old_oid))
		return 0;
	if (is_null_oid(&update->old_oid)) {
		strbuf_addf(err, _("cannot lock ref '%s': reference already exists"),
			    ref_update_original_update_refname(update));
		return REF_TRANSACTION_ERROR_CREATE_EXISTS;
	}
	if (current->target.type == VEX_REF_TARGET_SYMREF) {
		strbuf_addf(err, _("cannot lock ref '%s': is a symbolic ref"),
			    ref_update_original_update_refname(update));
		return REF_TRANSACTION_ERROR_INCORRECT_OLD_VALUE;
	}
	strbuf_addf(err, _("cannot lock ref '%s': is at %s but expected %s"),
		    ref_update_original_update_refname(update),
		    oid_to_hex(&current->target.oid), oid_to_hex(&update->old_oid));
	return REF_TRANSACTION_ERROR_INCORRECT_OLD_VALUE;
}

static int vex_transaction_has_update(struct ref_transaction *transaction,
				      struct ref_update *except, const char *refname)
{
	for (size_t i = 0; i < transaction->nr; i++) {
		struct ref_update *update = transaction->updates[i];

		if (update != except && !(update->flags & REF_LOG_ONLY) &&
		    !strcmp(update->refname, refname))
			return 1;
	}
	return 0;
}

static enum ref_transaction_error vex_prepare_update(
		struct vex_ref_store *refs, struct vex_transaction_data *data,
		struct ref_transaction *transaction, struct ref_update *update,
		struct strbuf *err)
{
	struct vex_ref current = {0};
	struct vex_ref_update vex_update = {
		.refname = update->refname,
		.is_fast_forward = -1,
		.push_id = "git-ref",
		.reason = "git ref transaction",
		.message = update->msg,
	};
	int exists, ret;

	ret = vex_read_current_ref(refs, update->refname, &current, &exists);
	if (ret) {
		strbuf_addf(err, _("unable to read Vex ref '%s'"), update->refname);
		return REF_TRANSACTION_ERROR_GENERIC;
	}

	if (exists && current.target.type == VEX_REF_TARGET_SYMREF)
		update->type |= REF_ISSYMREF;
	if (exists && current.target.type == VEX_REF_TARGET_SYMREF &&
	    !(update->flags & (REF_NO_DEREF | REF_LOG_ONLY))) {
		struct ref_update *child;

		if (vex_transaction_has_update(transaction, update,
					       current.target.symref)) {
			strbuf_addf(err, _("multiple updates for '%s' (including one via symref '%s') are not allowed"),
				    current.target.symref, update->refname);
			ret = REF_TRANSACTION_ERROR_NAME_CONFLICT;
			goto out;
		}
		child = ref_transaction_add_update(transaction, current.target.symref,
				update->flags | REF_NO_DEREF,
				update->new_target ? NULL : &update->new_oid,
				update->old_target ? NULL : &update->old_oid,
				&update->peeled, update->new_target, update->old_target,
				update->committer_info, update->msg);
		child->parent_update = update;
		update->flags |= REF_LOG_ONLY | REF_NO_DEREF;
		ret = 0;
		goto out;
	}

	ret = vex_validate_old_ref(update, &current, exists, err);
	if (ret)
		goto out;
	if (!(update->flags & REF_HAVE_NEW) || update->flags & REF_LOG_ONLY) {
		ret = 0;
		goto out;
	}

	vex_target_from_ref(&vex_update.old_target, &current, exists);
	vex_update.expected_version = exists ? current.version : 0;
	if (update->new_target) {
		vex_update.new_target.type = VEX_REF_TARGET_SYMREF;
		vex_update.new_target.symref = update->new_target;
	} else if (ref_update_has_null_new_value(update)) {
		vex_update.new_target.type = VEX_REF_TARGET_NONE;
	} else {
		vex_update.new_target.type = VEX_REF_TARGET_OID;
		oidcpy(&vex_update.new_target.oid, &update->new_oid);
	}
	ret = vex_ref_transaction_update(data->transaction, &vex_update);
	if (ret) {
		strbuf_addf(err, _("unable to stage Vex ref update for '%s'"),
			    update->refname);
		ret = REF_TRANSACTION_ERROR_GENERIC;
	}

out:
	vex_ref_clear(&current);
	return ret;
}

static int vex_be_transaction_prepare(struct ref_store *ref_store,
				      struct ref_transaction *transaction,
				      struct strbuf *err)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	struct vex_transaction_data *data = xcalloc(1, sizeof(*data));
	int ret;

	ret = odb_vex_receive_transaction_attach(
		refs->base.repo->objects->transaction, refs->store, &data->epoch);
	if (ret < 0) {
		strbuf_addstr(err, _("unable to join Vex receive object epoch"));
		goto fail;
	}
	if (ret > 0) {
		data->receive_transaction = refs->base.repo->objects->transaction;
		ret = 0;
	} else {
		ret = vex_epoch_allocate(refs->store, "git-ref", &data->epoch);
	}
	if (!ret)
		ret = vex_ref_transaction_begin(refs->store, data->epoch,
						&data->transaction);
	if (ret) {
		strbuf_addstr(err, _("unable to begin Vex ref transaction"));
		goto fail;
	}

	for (size_t i = 0; i < transaction->nr; i++) {
		ret = vex_prepare_update(refs, data, transaction,
					 transaction->updates[i], err);
		if (ret)
			goto fail;
	}

	transaction->backend_data = data;
	transaction->state = REF_TRANSACTION_PREPARED;
	return 0;

fail:
	if (data->transaction) {
		vex_ref_transaction_abort(data->transaction);
		data->transaction = NULL;
		if (data->receive_transaction)
			odb_vex_receive_transaction_finish(data->receive_transaction);
	} else if (data->receive_transaction) {
		odb_vex_receive_transaction_abort(data->receive_transaction);
	}
	if (data->epoch)
		vex_epoch_free(data->epoch);
	free(data);
	transaction->state = REF_TRANSACTION_CLOSED;
	return ret < 0 ? ret : REF_TRANSACTION_ERROR_GENERIC;
}

static int vex_be_transaction_abort(struct ref_store *ref_store UNUSED,
				    struct ref_transaction *transaction,
				    struct strbuf *err UNUSED)
{
	struct vex_transaction_data *data = transaction->backend_data;

	if (data) {
		if (data->transaction) {
			vex_ref_transaction_abort(data->transaction);
			data->transaction = NULL;
			if (data->receive_transaction)
				odb_vex_receive_transaction_finish(data->receive_transaction);
		} else if (data->receive_transaction) {
			odb_vex_receive_transaction_abort(data->receive_transaction);
		}
		if (data->epoch)
			vex_epoch_free(data->epoch);
		free(data);
	}
	transaction->backend_data = NULL;
	transaction->state = REF_TRANSACTION_CLOSED;
	return 0;
}

static int vex_be_transaction_finish(struct ref_store *ref_store UNUSED,
				     struct ref_transaction *transaction,
				     struct strbuf *err)
{
	struct vex_transaction_data *data = transaction->backend_data;
	int ret;

	ret = vex_ref_transaction_commit(data->transaction);
	/* commit retains the transaction allocation; abort releases it after commit. */
	vex_ref_transaction_abort(data->transaction);
	data->transaction = NULL;
	if (data->receive_transaction)
		odb_vex_receive_transaction_finish(data->receive_transaction);
	if (data->epoch)
		vex_epoch_free(data->epoch);
	free(data);
	transaction->backend_data = NULL;
	transaction->state = REF_TRANSACTION_CLOSED;
	if (ret) {
		strbuf_addstr(err, _("Vex ref transaction failed"));
		return -1;
	}
	return 0;
}

static int vex_be_optimize(struct ref_store *ref_store UNUSED,
			   struct refs_optimize_opts *opts UNUSED)
{
	return 0;
}

static int vex_be_optimize_required(struct ref_store *ref_store UNUSED,
				    struct refs_optimize_opts *opts UNUSED,
				    bool *required)
{
	*required = false;
	return 0;
}

static int vex_be_copy_or_rename(struct ref_store *ref_store,
				 const char *oldref, const char *newref,
				 const char *logmsg, int delete_old)
{
	struct vex_ref_store *refs = vex_downcast(ref_store);
	struct vex_ref old = {0};
	struct ref_transaction *transaction;
	struct strbuf err = STRBUF_INIT;
	int ret;

	ret = vex_ref_read(refs->store, oldref, &old);
	if (ret) {
		ret = error(_("refname %s not found"), oldref);
		goto out;
	}
	transaction = ref_store_transaction_begin(ref_store, 0, &err);
	if (!transaction) {
		ret = -1;
		goto out;
	}
	if (old.target.type == VEX_REF_TARGET_SYMREF)
		ret = ref_transaction_create(transaction, newref, NULL,
					     old.target.symref, REF_NO_DEREF, logmsg, &err);
	else
		ret = ref_transaction_create(transaction, newref, &old.target.oid,
					     NULL, REF_NO_DEREF, logmsg, &err);
	if (!ret && delete_old)
		ret = ref_transaction_delete(transaction, oldref, NULL, NULL,
					     REF_NO_DEREF, logmsg, &err);
	if (!ret)
		ret = ref_transaction_commit(transaction, &err);
	if (ret)
		error("%s", err.buf);
	ref_transaction_free(transaction);
out:
	vex_ref_clear(&old);
	strbuf_release(&err);
	return ret;
}

static int vex_be_rename_ref(struct ref_store *ref_store, const char *oldref,
				     const char *newref, const char *logmsg)
{
	return vex_be_copy_or_rename(ref_store, oldref, newref, logmsg, 1);
}

static int vex_be_copy_ref(struct ref_store *ref_store, const char *oldref,
				   const char *newref, const char *logmsg)
{
	return vex_be_copy_or_rename(ref_store, oldref, newref, logmsg, 0);
}

static struct ref_iterator *vex_be_reflog_iterator_begin(struct ref_store *ref_store UNUSED)
{
	return empty_ref_iterator_begin();
}

static int vex_be_for_each_reflog_ent(struct ref_store *ref_store UNUSED,
				      const char *refname UNUSED,
				      each_reflog_ent_fn fn UNUSED, void *cb_data UNUSED)
{
	return 0;
}

static int vex_be_reflog_exists(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED)
{
	return 0;
}

static int vex_be_create_reflog(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED, struct strbuf *err UNUSED)
{
	return 0;
}

static int vex_be_delete_reflog(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED)
{
	return 0;
}

static int vex_be_reflog_expire(struct ref_store *ref_store UNUSED,
				const char *refname UNUSED,
				unsigned int flags UNUSED,
				reflog_expiry_prepare_fn prepare_fn UNUSED,
				reflog_expiry_should_prune_fn should_prune_fn UNUSED,
				reflog_expiry_cleanup_fn cleanup_fn,
				void *policy_cb_data)
{
	if (cleanup_fn)
		cleanup_fn(policy_cb_data);
	return 0;
}

static int vex_be_fsck(struct ref_store *ref_store UNUSED,
		       struct fsck_options *o UNUSED, struct worktree *wt UNUSED)
{
	return 0;
}

struct ref_storage_be refs_be_vex = {
	.name = "vex",
	.init = vex_be_init,
	.release = vex_be_release,
	.create_on_disk = vex_be_create_on_disk,
	.remove_on_disk = vex_be_remove_on_disk,

	.transaction_prepare = vex_be_transaction_prepare,
	.transaction_finish = vex_be_transaction_finish,
	.transaction_abort = vex_be_transaction_abort,

	.optimize = vex_be_optimize,
	.optimize_required = vex_be_optimize_required,
	.rename_ref = vex_be_rename_ref,
	.copy_ref = vex_be_copy_ref,

	.iterator_begin = vex_be_iterator_begin,
	.read_raw_ref = vex_be_read_raw_ref,
	.read_symbolic_ref = vex_be_read_symbolic_ref,

	.reflog_iterator_begin = vex_be_reflog_iterator_begin,
	.for_each_reflog_ent = vex_be_for_each_reflog_ent,
	.for_each_reflog_ent_reverse = vex_be_for_each_reflog_ent,
	.reflog_exists = vex_be_reflog_exists,
	.create_reflog = vex_be_create_reflog,
	.delete_reflog = vex_be_delete_reflog,
	.reflog_expire = vex_be_reflog_expire,

	.fsck = vex_be_fsck,
};
