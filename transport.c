#include "cache.h"
#include "transport.h"
#include "run-command.h"
#include "pkt-line.h"
#include "fetch-pack.h"
#include "remote.h"
#include "connect.h"
#include "send-pack.h"
#include "walker.h"
#include "bundle.h"
#include "dir.h"
#include "refs.h"
#include "branch.h"
#include "url.h"
#include "submodule.h"
#include "string-list.h"
#include "sha1-array.h"
#include "sigchain.h"

static void set_upstreams(struct transport *transport, struct ref *refs,
	int pretend)
{
	struct ref *ref;
	for (ref = refs; ref; ref = ref->next) {
		const char *localname;
		const char *tmp;
		const char *remotename;
		unsigned char sha[20];
		int flag = 0;
		/*
		 * Check suitability for tracking. Must be successful /
		 * already up-to-date ref create/modify (not delete).
		 */
		if (ref->status != REF_STATUS_OK &&
			ref->status != REF_STATUS_UPTODATE)
			continue;
		if (!ref->peer_ref)
			continue;
		if (is_null_oid(&ref->new_oid))
			continue;

		/* Follow symbolic refs (mainly for HEAD). */
		localname = ref->peer_ref->name;
		remotename = ref->name;
		tmp = resolve_ref_unsafe(localname, RESOLVE_REF_READING,
					 sha, &flag);
		if (tmp && flag & REF_ISSYMREF &&
			starts_with(tmp, "refs/heads/"))
			localname = tmp;

		/* Both source and destination must be local branches. */
		if (!localname || !starts_with(localname, "refs/heads/"))
			continue;
		if (!remotename || !starts_with(remotename, "refs/heads/"))
			continue;

		if (!pretend)
			install_branch_config(BRANCH_CONFIG_VERBOSE,
				localname + 11, transport->remote->name,
				remotename);
		else
			printf(_("Would set upstream of '%s' to '%s' of '%s'\n"),
				localname + 11, remotename + 11,
				transport->remote->name);
	}
}

struct bundle_transport_data {
	int fd;
	struct bundle_header header;
};

static struct ref *get_refs_from_bundle(struct transport *transport, int for_push)
{
	struct bundle_transport_data *data = transport->data;
	struct ref *result = NULL;
	int i;

	if (for_push)
		return NULL;

	if (data->fd > 0)
		close(data->fd);
	data->fd = read_bundle_header(transport->url, &data->header);
	if (data->fd < 0)
		die ("Could not read bundle '%s'.", transport->url);
	for (i = 0; i < data->header.references.nr; i++) {
		struct ref_list_entry *e = data->header.references.list + i;
		struct ref *ref = alloc_ref(e->name);
		hashcpy(ref->old_oid.hash, e->sha1);
		ref->next = result;
		result = ref;
	}
	return result;
}

static int fetch_refs_from_bundle(struct transport *transport,
			       int nr_heads, struct ref **to_fetch)
{
	struct bundle_transport_data *data = transport->data;
	return unbundle(&data->header, data->fd,
			transport->progress ? BUNDLE_VERBOSE : 0);
}

static int close_bundle(struct transport *transport)
{
	struct bundle_transport_data *data = transport->data;
	if (data->fd > 0)
		close(data->fd);
	free(data);
	return 0;
}

struct git_transport_data {
	struct git_transport_options options;
	struct child_process *conn;
	int fd[2];
	unsigned got_remote_heads : 1;
	struct sha1_array extra_have;
	struct sha1_array shallow;
};

static int set_git_option(struct git_transport_options *opts,
			  const char *name, const char *value)
{
	if (!strcmp(name, TRANS_OPT_UPLOADPACK)) {
		opts->uploadpack = value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_RECEIVEPACK)) {
		opts->receivepack = value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_THIN)) {
		opts->thin = !!value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_FOLLOWTAGS)) {
		opts->followtags = !!value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_KEEP)) {
		opts->keep = !!value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_UPDATE_SHALLOW)) {
		opts->update_shallow = !!value;
		return 0;
	} else if (!strcmp(name, TRANS_OPT_DEPTH)) {
		if (!value)
			opts->depth = 0;
		else {
			char *end;
			opts->depth = strtol(value, &end, 0);
			if (*end)
				die(_("transport: invalid depth option '%s'"), value);
		}
		return 0;
	}
	return 1;
}

static int connect_setup(struct transport *transport, int for_push)
{
	struct git_transport_data *data = transport->data;
	int flags = transport->verbose > 0 ? CONNECT_VERBOSE : 0;

	if (data->conn)
		return 0;

	switch (transport->family) {
	case TRANSPORT_FAMILY_ALL: break;
	case TRANSPORT_FAMILY_IPV4: flags |= CONNECT_IPV4; break;
	case TRANSPORT_FAMILY_IPV6: flags |= CONNECT_IPV6; break;
	}

	data->conn = git_connect(data->fd, transport->url,
				 for_push ? data->options.receivepack :
				 data->options.uploadpack,
				 flags);

	return 0;
}

static struct ref *get_refs_via_connect(struct transport *transport, int for_push)
{
	struct git_transport_data *data = transport->data;
	struct ref *refs;

	connect_setup(transport, for_push);
	get_remote_heads(data->fd[0], NULL, 0, &refs,
			 for_push ? REF_NORMAL : 0,
			 &data->extra_have,
			 &data->shallow);
	data->got_remote_heads = 1;

	return refs;
}

static int fetch_refs_via_pack(struct transport *transport,
			       int nr_heads, struct ref **to_fetch)
{
	struct git_transport_data *data = transport->data;
	struct ref *refs;
	char *dest = xstrdup(transport->url);
	struct fetch_pack_args args;
	struct ref *refs_tmp = NULL;

	memset(&args, 0, sizeof(args));
	args.uploadpack = data->options.uploadpack;
	args.keep_pack = data->options.keep;
	args.lock_pack = 1;
	args.use_thin_pack = data->options.thin;
	args.include_tag = data->options.followtags;
	args.verbose = (transport->verbose > 1);
	args.quiet = (transport->verbose < 0);
	args.no_progress = !transport->progress;
	args.depth = data->options.depth;
	args.check_self_contained_and_connected =
		data->options.check_self_contained_and_connected;
	args.cloning = transport->cloning;
	args.update_shallow = data->options.update_shallow;

	if (!data->got_remote_heads) {
		connect_setup(transport, 0);
		get_remote_heads(data->fd[0], NULL, 0, &refs_tmp, 0,
				 NULL, &data->shallow);
		data->got_remote_heads = 1;
	}

	refs = fetch_pack(&args, data->fd, data->conn,
			  refs_tmp ? refs_tmp : transport->remote_refs,
			  dest, to_fetch, nr_heads, &data->shallow,
			  &transport->pack_lockfile);
	close(data->fd[0]);
	close(data->fd[1]);
	if (finish_connect(data->conn)) {
		free_refs(refs);
		refs = NULL;
	}
	data->conn = NULL;
	data->got_remote_heads = 0;
	data->options.self_contained_and_connected =
		args.self_contained_and_connected;

	free_refs(refs_tmp);
	free_refs(refs);
	free(dest);
	return (refs ? 0 : -1);
}

static int push_had_errors(struct ref *ref)
{
	for (; ref; ref = ref->next) {
		switch (ref->status) {
		case REF_STATUS_NONE:
		case REF_STATUS_UPTODATE:
		case REF_STATUS_OK:
			break;
		default:
			return 1;
		}
	}
	return 0;
}

int transport_refs_pushed(struct ref *ref)
{
	for (; ref; ref = ref->next) {
		switch(ref->status) {
		case REF_STATUS_NONE:
		case REF_STATUS_UPTODATE:
			break;
		default:
			return 1;
		}
	}
	return 0;
}

void transport_update_tracking_ref(struct remote *remote, struct ref *ref, int verbose)
{
	struct refspec rs;

	if (ref->status != REF_STATUS_OK && ref->status != REF_STATUS_UPTODATE)
		return;

	rs.src = ref->name;
	rs.dst = NULL;

	if (!remote_find_tracking(remote, &rs)) {
		if (verbose)
			fprintf(stderr, "updating local tracking ref '%s'\n", rs.dst);
		if (ref->deletion) {
			delete_ref(rs.dst, NULL, 0);
		} else
			update_ref("update by push", rs.dst,
					ref->new_oid.hash, NULL, 0, 0);
		free(rs.dst);
	}
}

static void print_ref_status(char flag, const char *summary, struct ref *to, struct ref *from, const char *msg, int porcelain)
{
	if (porcelain) {
		if (from)
			fprintf(stdout, "%c\t%s:%s\t", flag, from->name, to->name);
		else
			fprintf(stdout, "%c\t:%s\t", flag, to->name);
		if (msg)
			fprintf(stdout, "%s (%s)\n", summary, msg);
		else
			fprintf(stdout, "%s\n", summary);
	} else {
		fprintf(stderr, " %c %-*s ", flag, TRANSPORT_SUMMARY_WIDTH, summary);
		if (from)
			fprintf(stderr, "%s -> %s", prettify_refname(from->name), prettify_refname(to->name));
		else
			fputs(prettify_refname(to->name), stderr);
		if (msg) {
			fputs(" (", stderr);
			fputs(msg, stderr);
			fputc(')', stderr);
		}
		fputc('\n', stderr);
	}
}

static void print_ok_ref_status(struct ref *ref, int porcelain)
{
	if (ref->deletion)
		print_ref_status('-', "[deleted]", ref, NULL, NULL, porcelain);
	else if (is_null_oid(&ref->old_oid))
		print_ref_status('*',
			(starts_with(ref->name, "refs/tags/") ? "[new tag]" :
			"[new branch]"),
			ref, ref->peer_ref, NULL, porcelain);
	else {
		struct strbuf quickref = STRBUF_INIT;
		char type;
		const char *msg;

		strbuf_add_unique_abbrev(&quickref, ref->old_oid.hash,
					 DEFAULT_ABBREV);
		if (ref->forced_update) {
			strbuf_addstr(&quickref, "...");
			type = '+';
			msg = "forced update";
		} else {
			strbuf_addstr(&quickref, "..");
			type = ' ';
			msg = NULL;
		}
		strbuf_add_unique_abbrev(&quickref, ref->new_oid.hash,
					 DEFAULT_ABBREV);

		print_ref_status(type, quickref.buf, ref, ref->peer_ref, msg, porcelain);
		strbuf_release(&quickref);
	}
}

static int print_one_push_status(struct ref *ref, const char *dest, int count, int porcelain)
{
	if (!count) {
		char *url = transport_anonymize_url(dest);
		fprintf(porcelain ? stdout : stderr, "To %s\n", url);
		free(url);
	}

	switch(ref->status) {
	case REF_STATUS_NONE:
		print_ref_status('X', "[no match]", ref, NULL, NULL, porcelain);
		break;
	case REF_STATUS_REJECT_NODELETE:
		print_ref_status('!', "[rejected]", ref, NULL,
						 "remote does not support deleting refs", porcelain);
		break;
	case REF_STATUS_UPTODATE:
		print_ref_status('=', "[up to date]", ref,
						 ref->peer_ref, NULL, porcelain);
		break;
	case REF_STATUS_REJECT_NONFASTFORWARD:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "non-fast-forward", porcelain);
		break;
	case REF_STATUS_REJECT_ALREADY_EXISTS:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "already exists", porcelain);
		break;
	case REF_STATUS_REJECT_FETCH_FIRST:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "fetch first", porcelain);
		break;
	case REF_STATUS_REJECT_NEEDS_FORCE:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "needs force", porcelain);
		break;
	case REF_STATUS_REJECT_STALE:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "stale info", porcelain);
		break;
	case REF_STATUS_REJECT_SHALLOW:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "new shallow roots not allowed", porcelain);
		break;
	case REF_STATUS_REMOTE_REJECT:
		print_ref_status('!', "[remote rejected]", ref,
						 ref->deletion ? NULL : ref->peer_ref,
						 ref->remote_status, porcelain);
		break;
	case REF_STATUS_EXPECTING_REPORT:
		print_ref_status('!', "[remote failure]", ref,
						 ref->deletion ? NULL : ref->peer_ref,
						 "remote failed to report status", porcelain);
		break;
	case REF_STATUS_ATOMIC_PUSH_FAILED:
		print_ref_status('!', "[rejected]", ref, ref->peer_ref,
						 "atomic push failed", porcelain);
		break;
	case REF_STATUS_OK:
		print_ok_ref_status(ref, porcelain);
		break;
	}

	return 1;
}

void transport_print_push_status(const char *dest, struct ref *refs,
				  int verbose, int porcelain, unsigned int *reject_reasons)
{
	struct ref *ref;
	int n = 0;
	unsigned char head_sha1[20];
	char *head;

	head = resolve_refdup("HEAD", RESOLVE_REF_READING, head_sha1, NULL);

	if (verbose) {
		for (ref = refs; ref; ref = ref->next)
			if (ref->status == REF_STATUS_UPTODATE)
				n += print_one_push_status(ref, dest, n, porcelain);
	}

	for (ref = refs; ref; ref = ref->next)
		if (ref->status == REF_STATUS_OK)
			n += print_one_push_status(ref, dest, n, porcelain);

	*reject_reasons = 0;
	for (ref = refs; ref; ref = ref->next) {
		if (ref->status != REF_STATUS_NONE &&
		    ref->status != REF_STATUS_UPTODATE &&
		    ref->status != REF_STATUS_OK)
			n += print_one_push_status(ref, dest, n, porcelain);
		if (ref->status == REF_STATUS_REJECT_NONFASTFORWARD) {
			if (head != NULL && !strcmp(head, ref->name))
				*reject_reasons |= REJECT_NON_FF_HEAD;
			else
				*reject_reasons |= REJECT_NON_FF_OTHER;
		} else if (ref->status == REF_STATUS_REJECT_ALREADY_EXISTS) {
			*reject_reasons |= REJECT_ALREADY_EXISTS;
		} else if (ref->status == REF_STATUS_REJECT_FETCH_FIRST) {
			*reject_reasons |= REJECT_FETCH_FIRST;
		} else if (ref->status == REF_STATUS_REJECT_NEEDS_FORCE) {
			*reject_reasons |= REJECT_NEEDS_FORCE;
		}
	}
	free(head);
}

void transport_verify_remote_names(int nr_heads, const char **heads)
{
	int i;

	for (i = 0; i < nr_heads; i++) {
		const char *local = heads[i];
		const char *remote = strrchr(heads[i], ':');

		if (*local == '+')
			local++;

		/* A matching refspec is okay.  */
		if (remote == local && remote[1] == '\0')
			continue;

		remote = remote ? (remote + 1) : local;
		if (check_refname_format(remote,
				REFNAME_ALLOW_ONELEVEL|REFNAME_REFSPEC_PATTERN))
			die("remote part of refspec is not a valid name in %s",
				heads[i]);
	}
}

static int git_transport_push(struct transport *transport, struct ref *remote_refs, int flags)
{
	struct git_transport_data *data = transport->data;
	struct send_pack_args args;
	int ret;

	if (!data->got_remote_heads) {
		struct ref *tmp_refs;
		connect_setup(transport, 1);

		get_remote_heads(data->fd[0], NULL, 0, &tmp_refs, REF_NORMAL,
				 NULL, &data->shallow);
		data->got_remote_heads = 1;
	}

	memset(&args, 0, sizeof(args));
	args.send_mirror = !!(flags & TRANSPORT_PUSH_MIRROR);
	args.force_update = !!(flags & TRANSPORT_PUSH_FORCE);
	args.use_thin_pack = data->options.thin;
	args.verbose = (transport->verbose > 0);
	args.quiet = (transport->verbose < 0);
	args.progress = transport->progress;
	args.dry_run = !!(flags & TRANSPORT_PUSH_DRY_RUN);
	args.porcelain = !!(flags & TRANSPORT_PUSH_PORCELAIN);
	args.atomic = !!(flags & TRANSPORT_PUSH_ATOMIC);
	args.push_options = transport->push_options;
	args.url = transport->url;

	if (flags & TRANSPORT_PUSH_CERT_ALWAYS)
		args.push_cert = SEND_PACK_PUSH_CERT_ALWAYS;
	else if (flags & TRANSPORT_PUSH_CERT_IF_ASKED)
		args.push_cert = SEND_PACK_PUSH_CERT_IF_ASKED;
	else
		args.push_cert = SEND_PACK_PUSH_CERT_NEVER;

	ret = send_pack(&args, data->fd, data->conn, remote_refs,
			&data->extra_have);

	close(data->fd[1]);
	close(data->fd[0]);
	ret |= finish_connect(data->conn);
	data->conn = NULL;
	data->got_remote_heads = 0;

	return ret;
}

static int connect_git(struct transport *transport, const char *name,
		       const char *executable, int fd[2])
{
	struct git_transport_data *data = transport->data;
	data->conn = git_connect(data->fd, transport->url,
				 executable, 0);
	fd[0] = data->fd[0];
	fd[1] = data->fd[1];
	return 0;
}

static int disconnect_git(struct transport *transport)
{
	struct git_transport_data *data = transport->data;
	if (data->conn) {
		if (data->got_remote_heads)
			packet_flush(data->fd[1]);
		close(data->fd[0]);
		close(data->fd[1]);
		finish_connect(data->conn);
	}

	free(data);
	return 0;
}

void transport_take_over(struct transport *transport,
			 struct child_process *child)
{
	struct git_transport_data *data;

	if (!transport->smart_options)
		die("BUG: taking over transport requires non-NULL "
		    "smart_options field.");

	data = xcalloc(1, sizeof(*data));
	data->options = *transport->smart_options;
	data->conn = child;
	data->fd[0] = data->conn->out;
	data->fd[1] = data->conn->in;
	data->got_remote_heads = 0;
	transport->data = data;

	transport->set_option = NULL;
	transport->get_refs_list = get_refs_via_connect;
	transport->fetch = fetch_refs_via_pack;
	transport->push = NULL;
	transport->push_refs = git_transport_push;
	transport->disconnect = disconnect_git;
	transport->smart_options = &(data->options);

	transport->cannot_reuse = 1;
}

static int is_file(const char *url)
{
	struct stat buf;
	if (stat(url, &buf))
		return 0;
	return S_ISREG(buf.st_mode);
}

static int external_specification_len(const char *url)
{
	return strchr(url, ':') - url;
}

static const struct string_list *protocol_whitelist(void)
{
	static int enabled = -1;
	static struct string_list allowed = STRING_LIST_INIT_DUP;

	if (enabled < 0) {
		const char *v = getenv("GIT_ALLOW_PROTOCOL");
		if (v) {
			string_list_split(&allowed, v, ':', -1);
			string_list_sort(&allowed);
			enabled = 1;
		} else {
			enabled = 0;
		}
	}

	return enabled ? &allowed : NULL;
}

int is_transport_allowed(const char *type)
{
	const struct string_list *allowed = protocol_whitelist();
	return !allowed || string_list_has_string(allowed, type);
}

void transport_check_allowed(const char *type)
{
	if (!is_transport_allowed(type))
		die("transport '%s' not allowed", type);
}

int transport_restrict_protocols(void)
{
	return !!protocol_whitelist();
}

struct transport *transport_get(struct remote *remote, const char *url)
{
	const char *helper;
	struct transport *ret = xcalloc(1, sizeof(*ret));

	ret->progress = isatty(2);

	if (!remote)
		die("No remote provided to transport_get()");

	ret->got_remote_refs = 0;
	ret->remote = remote;
	helper = remote->foreign_vcs;

	if (!url && remote->url)
		url = remote->url[0];
	ret->url = url;

	/* maybe it is a foreign URL? */
	if (url) {
		const char *p = url;

		while (is_urlschemechar(p == url, *p))
			p++;
		if (starts_with(p, "::"))
			helper = xstrndup(url, p - url);
	}

	if (helper) {
		transport_helper_init(ret, helper);
	} else if (starts_with(url, "rsync:")) {
		die("git-over-rsync is no longer supported");
	} else if (url_is_local_not_ssh(url) && is_file(url) && is_bundle(url, 1)) {
		struct bundle_transport_data *data = xcalloc(1, sizeof(*data));
		transport_check_allowed("file");
		ret->data = data;
		ret->get_refs_list = get_refs_from_bundle;
		ret->fetch = fetch_refs_from_bundle;
		ret->disconnect = close_bundle;
		ret->smart_options = NULL;
	} else if (!is_url(url)
		|| starts_with(url, "file://")
		|| starts_with(url, "git://")
		|| starts_with(url, "ssh://")
		|| starts_with(url, "git+ssh://") /* deprecated - do not use */
		|| starts_with(url, "ssh+git://") /* deprecated - do not use */
		) {
		/*
		 * These are builtin smart transports; "allowed" transports
		 * will be checked individually in git_connect.
		 */
		struct git_transport_data *data = xcalloc(1, sizeof(*data));
		ret->data = data;
		ret->set_option = NULL;
		ret->get_refs_list = get_refs_via_connect;
		ret->fetch = fetch_refs_via_pack;
		ret->push_refs = git_transport_push;
		ret->connect = connect_git;
		ret->disconnect = disconnect_git;
		ret->smart_options = &(data->options);

		data->conn = NULL;
		data->got_remote_heads = 0;
	} else {
		/* Unknown protocol in URL. Pass to external handler. */
		int len = external_specification_len(url);
		char *handler = xmemdupz(url, len);
		transport_helper_init(ret, handler);
	}

	if (ret->smart_options) {
		ret->smart_options->thin = 1;
		ret->smart_options->uploadpack = "git-upload-pack";
		if (remote->uploadpack)
			ret->smart_options->uploadpack = remote->uploadpack;
		ret->smart_options->receivepack = "git-receive-pack";
		if (remote->receivepack)
			ret->smart_options->receivepack = remote->receivepack;
	}

	return ret;
}

int transport_set_option(struct transport *transport,
			 const char *name, const char *value)
{
	int git_reports = 1, protocol_reports = 1;

	if (transport->smart_options)
		git_reports = set_git_option(transport->smart_options,
					     name, value);

	if (transport->set_option)
		protocol_reports = transport->set_option(transport, name,
							value);

	/* If either report is 0, report 0 (success). */
	if (!git_reports || !protocol_reports)
		return 0;
	/* If either reports -1 (invalid value), report -1. */
	if ((git_reports == -1) || (protocol_reports == -1))
		return -1;
	/* Otherwise if both report unknown, report unknown. */
	return 1;
}

void transport_set_verbosity(struct transport *transport, int verbosity,
	int force_progress)
{
	if (verbosity >= 1)
		transport->verbose = verbosity <= 3 ? verbosity : 3;
	if (verbosity < 0)
		transport->verbose = -1;

	/**
	 * Rules used to determine whether to report progress (processing aborts
	 * when a rule is satisfied):
	 *
	 *   . Report progress, if force_progress is 1 (ie. --progress).
	 *   . Don't report progress, if force_progress is 0 (ie. --no-progress).
	 *   . Don't report progress, if verbosity < 0 (ie. -q/--quiet ).
	 *   . Report progress if isatty(2) is 1.
	 **/
	if (force_progress >= 0)
		transport->progress = !!force_progress;
	else
		transport->progress = verbosity >= 0 && isatty(2);
}

static void die_with_unpushed_submodules(struct string_list *needs_pushing)
{
	int i;

	fprintf(stderr, _("The following submodule paths contain changes that can\n"
			"not be found on any remote:\n"));
	for (i = 0; i < needs_pushing->nr; i++)
		printf("  %s\n", needs_pushing->items[i].string);
	fprintf(stderr, _("\nPlease try\n\n"
			  "	git push --recurse-submodules=on-demand\n\n"
			  "or cd to the path and use\n\n"
			  "	git push\n\n"
			  "to push them to a remote.\n\n"));

	string_list_clear(needs_pushing, 0);

	die(_("Aborting."));
}

static int run_pre_push_hook(struct transport *transport,
			     struct ref *remote_refs)
{
	int ret = 0, x;
	struct ref *r;
	struct child_process proc = CHILD_PROCESS_INIT;
	struct strbuf buf;
	const char *argv[4];

	if (!(argv[0] = find_hook("pre-push")))
		return 0;

	argv[1] = transport->remote->name;
	argv[2] = transport->url;
	argv[3] = NULL;

	proc.argv = argv;
	proc.in = -1;

	if (start_command(&proc)) {
		finish_command(&proc);
		return -1;
	}

	sigchain_push(SIGPIPE, SIG_IGN);

	strbuf_init(&buf, 256);

	for (r = remote_refs; r; r = r->next) {
		if (!r->peer_ref) continue;
		if (r->status == REF_STATUS_REJECT_NONFASTFORWARD) continue;
		if (r->status == REF_STATUS_REJECT_STALE) continue;
		if (r->status == REF_STATUS_UPTODATE) continue;

		strbuf_reset(&buf);
		strbuf_addf( &buf, "%s %s %s %s\n",
			 r->peer_ref->name, oid_to_hex(&r->new_oid),
			 r->name, oid_to_hex(&r->old_oid));

		if (write_in_full(proc.in, buf.buf, buf.len) < 0) {
			/* We do not mind if a hook does not read all refs. */
			if (errno != EPIPE)
				ret = -1;
			break;
		}
	}

	strbuf_release(&buf);

	x = close(proc.in);
	if (!ret)
		ret = x;

	sigchain_pop(SIGPIPE);

	x = finish_command(&proc);
	if (!ret)
		ret = x;

	return ret;
}

static void collect_new_old_hashes(struct ref *ref, struct sha1_array *new_hashes,
		struct sha1_array *old_hashes)
{
	for (; ref; ref = ref->next) {
		if (!is_null_oid(&ref->new_oid)) {
			/* Just need to handle non-null oid's. If there
			 * is no new we are handling a deletion and do
			 * not need to check
			 */
			sha1_array_append(new_hashes, ref->new_oid.hash);

			/* if the old id is null we are handling an new
			 * ref and can simply skip appending the oid
			 * since they are used to reduce the amount of
			 * checked revisions.
			 */
			if (!is_null_oid(&ref->old_oid))
				sha1_array_append(old_hashes, ref->old_oid.hash);
		}
	}
}

int transport_push(struct transport *transport,
		   int refspec_nr, const char **refspec, int flags,
		   unsigned int *reject_reasons)
{
	*reject_reasons = 0;
	transport_verify_remote_names(refspec_nr, refspec);

	if (transport->push) {
		/* Maybe FIXME. But no important transport uses this case. */
		if (flags & TRANSPORT_PUSH_SET_UPSTREAM)
			die("This transport does not support using --set-upstream");

		return transport->push(transport, refspec_nr, refspec, flags);
	} else if (transport->push_refs) {
		struct ref *remote_refs;
		struct ref *local_refs = get_local_heads();
		int match_flags = MATCH_REFS_NONE;
		int verbose = (transport->verbose > 0);
		int quiet = (transport->verbose < 0);
		int porcelain = flags & TRANSPORT_PUSH_PORCELAIN;
		int pretend = flags & TRANSPORT_PUSH_DRY_RUN;
		int push_ret, ret, err;

		if (check_push_refs(local_refs, refspec_nr, refspec) < 0)
			return -1;

		remote_refs = transport->get_refs_list(transport, 1);

		if (flags & TRANSPORT_PUSH_ALL)
			match_flags |= MATCH_REFS_ALL;
		if (flags & TRANSPORT_PUSH_MIRROR)
			match_flags |= MATCH_REFS_MIRROR;
		if (flags & TRANSPORT_PUSH_PRUNE)
			match_flags |= MATCH_REFS_PRUNE;
		if (flags & TRANSPORT_PUSH_FOLLOW_TAGS)
			match_flags |= MATCH_REFS_FOLLOW_TAGS;

		if (match_push_refs(local_refs, &remote_refs,
				    refspec_nr, refspec, match_flags)) {
			return -1;
		}

		if (transport->smart_options &&
		    transport->smart_options->cas &&
		    !is_empty_cas(transport->smart_options->cas))
			apply_push_cas(transport->smart_options->cas,
				       transport->remote, remote_refs);

		set_ref_status_for_push(remote_refs,
			flags & TRANSPORT_PUSH_MIRROR,
			flags & TRANSPORT_PUSH_FORCE);

		if (!(flags & TRANSPORT_PUSH_NO_HOOK))
			if (run_pre_push_hook(transport, remote_refs))
				return -1;

		if ((flags & TRANSPORT_RECURSE_SUBMODULES_ON_DEMAND) && !is_bare_repository()) {
			struct ref *ref = remote_refs;
			struct sha1_array new_hashes = SHA1_ARRAY_INIT;
			struct sha1_array old_hashes = SHA1_ARRAY_INIT;

			collect_new_old_hashes(ref, &new_hashes, &old_hashes);

			if (!push_unpushed_submodules(&old_hashes, &new_hashes))
				die ("Failed to push all needed submodules!");
		}

		if ((flags & (TRANSPORT_RECURSE_SUBMODULES_ON_DEMAND |
			      TRANSPORT_RECURSE_SUBMODULES_CHECK)) && !is_bare_repository()) {
			struct ref *ref = remote_refs;
			struct string_list needs_pushing = STRING_LIST_INIT_DUP;
			struct sha1_array new_hashes = SHA1_ARRAY_INIT;
			struct sha1_array old_hashes = SHA1_ARRAY_INIT;

			collect_new_old_hashes(ref, &new_hashes, &old_hashes);

			if (find_unpushed_submodules(&old_hashes, &new_hashes,
						&needs_pushing))
				die_with_unpushed_submodules(&needs_pushing);
		}

		push_ret = transport->push_refs(transport, remote_refs, flags);
		err = push_had_errors(remote_refs);
		ret = push_ret | err;

		if (!quiet || err)
			transport_print_push_status(transport->url, remote_refs,
					verbose | porcelain, porcelain,
					reject_reasons);

		if (flags & TRANSPORT_PUSH_SET_UPSTREAM)
			set_upstreams(transport, remote_refs, pretend);

		if (!(flags & TRANSPORT_PUSH_DRY_RUN)) {
			struct ref *ref;
			for (ref = remote_refs; ref; ref = ref->next)
				transport_update_tracking_ref(transport->remote, ref, verbose);
		}

		if (porcelain && !push_ret)
			puts("Done");
		else if (!quiet && !ret && !transport_refs_pushed(remote_refs))
			fprintf(stderr, "Everything up-to-date\n");

		return ret;
	}
	return 1;
}

const struct ref *transport_get_remote_refs(struct transport *transport)
{
	if (!transport->got_remote_refs) {
		transport->remote_refs = transport->get_refs_list(transport, 0);
		transport->got_remote_refs = 1;
	}

	return transport->remote_refs;
}

int transport_fetch_refs(struct transport *transport, struct ref *refs)
{
	int rc;
	int nr_heads = 0, nr_alloc = 0, nr_refs = 0;
	struct ref **heads = NULL;
	struct ref *rm;

	for (rm = refs; rm; rm = rm->next) {
		nr_refs++;
		if (rm->peer_ref &&
		    !is_null_oid(&rm->old_oid) &&
		    !oidcmp(&rm->peer_ref->old_oid, &rm->old_oid))
			continue;
		ALLOC_GROW(heads, nr_heads + 1, nr_alloc);
		heads[nr_heads++] = rm;
	}

	if (!nr_heads) {
		/*
		 * When deepening of a shallow repository is requested,
		 * then local and remote refs are likely to still be equal.
		 * Just feed them all to the fetch method in that case.
		 * This condition shouldn't be met in a non-deepening fetch
		 * (see builtin/fetch.c:quickfetch()).
		 */
		ALLOC_ARRAY(heads, nr_refs);
		for (rm = refs; rm; rm = rm->next)
			heads[nr_heads++] = rm;
	}

	rc = transport->fetch(transport, nr_heads, heads);

	free(heads);
	return rc;
}

void transport_unlock_pack(struct transport *transport)
{
	if (transport->pack_lockfile) {
		unlink_or_warn(transport->pack_lockfile);
		free(transport->pack_lockfile);
		transport->pack_lockfile = NULL;
	}
}

int transport_connect(struct transport *transport, const char *name,
		      const char *exec, int fd[2])
{
	if (transport->connect)
		return transport->connect(transport, name, exec, fd);
	else
		die("Operation not supported by protocol");
}

int transport_disconnect(struct transport *transport)
{
	int ret = 0;
	if (transport->disconnect)
		ret = transport->disconnect(transport);
	free(transport);
	return ret;
}

/*
 * Strip username (and password) from a URL and return
 * it in a newly allocated string.
 */
char *transport_anonymize_url(const char *url)
{
	char *scheme_prefix, *anon_part;
	size_t anon_len, prefix_len = 0;

	anon_part = strchr(url, '@');
	if (url_is_local_not_ssh(url) || !anon_part)
		goto literal_copy;

	anon_len = strlen(++anon_part);
	scheme_prefix = strstr(url, "://");
	if (!scheme_prefix) {
		if (!strchr(anon_part, ':'))
			/* cannot be "me@there:/path/name" */
			goto literal_copy;
	} else {
		const char *cp;
		/* make sure scheme is reasonable */
		for (cp = url; cp < scheme_prefix; cp++) {
			switch (*cp) {
				/* RFC 1738 2.1 */
			case '+': case '.': case '-':
				break; /* ok */
			default:
				if (isalnum(*cp))
					break;
				/* it isn't */
				goto literal_copy;
			}
		}
		/* @ past the first slash does not count */
		cp = strchr(scheme_prefix + 3, '/');
		if (cp && cp < anon_part)
			goto literal_copy;
		prefix_len = scheme_prefix - url + 3;
	}
	return xstrfmt("%.*s%.*s", (int)prefix_len, url,
		       (int)anon_len, anon_part);
literal_copy:
	return xstrdup(url);
}

struct alternate_refs_data {
	alternate_ref_fn *fn;
	void *data;
};

static int refs_from_alternate_cb(struct alternate_object_database *e,
				  void *data)
{
	char *other;
	size_t len;
	struct remote *remote;
	struct transport *transport;
	const struct ref *extra;
	struct alternate_refs_data *cb = data;

	e->name[-1] = '\0';
	other = xstrdup(real_path(e->base));
	e->name[-1] = '/';
	len = strlen(other);

	while (other[len-1] == '/')
		other[--len] = '\0';
	if (len < 8 || memcmp(other + len - 8, "/objects", 8))
		goto out;
	/* Is this a git repository with refs? */
	memcpy(other + len - 8, "/refs", 6);
	if (!is_directory(other))
		goto out;
	other[len - 8] = '\0';
	remote = remote_get(other);
	transport = transport_get(remote, other);
	for (extra = transport_get_remote_refs(transport);
	     extra;
	     extra = extra->next)
		cb->fn(extra, cb->data);
	transport_disconnect(transport);
out:
	free(other);
	return 0;
}

void for_each_alternate_ref(alternate_ref_fn fn, void *data)
{
	struct alternate_refs_data cb;
	cb.fn = fn;
	cb.data = data;
	foreach_alt_odb(refs_from_alternate_cb, &cb);
}
