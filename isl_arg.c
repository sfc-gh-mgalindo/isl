#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isl_arg.h"

static void set_default_choice(struct isl_arg *arg, void *opt)
{
	*(unsigned *)(((char *)opt) + arg->offset) = arg->u.choice.default_value;
}

static void set_default_bool(struct isl_arg *arg, void *opt)
{
	*(unsigned *)(((char *)opt) + arg->offset) = arg->u.b.default_value;
}

static void set_default_child(struct isl_arg *arg, void *opt)
{
	void *child = calloc(1, arg->u.child.size);

	if (child)
		isl_arg_set_defaults(arg->u.child.child, child);

	*(void **)(((char *)opt) + arg->offset) = child;
}

void isl_arg_set_defaults(struct isl_arg *arg, void *opt)
{
	int i;

	for (i = 0; arg[i].type != isl_arg_end; ++i) {
		switch (arg[i].type) {
		case isl_arg_choice:
			set_default_choice(&arg[i], opt);
			break;
		case isl_arg_bool:
			set_default_bool(&arg[i], opt);
			break;
		case isl_arg_child:
			set_default_child(&arg[i], opt);
			break;
		}
	}
}

static void print_arg_help(struct isl_arg *decl, const char *prefix)
{
	if (decl->short_name)
		printf("  -%c, --", decl->short_name);
	else
		printf("      --");
	if (prefix)
		printf("%s-", prefix);
	printf("%s", decl->long_name);
}

static void print_choice_help(struct isl_arg *decl, const char *prefix)
{
	int i;

	print_arg_help(decl, prefix);
	printf("=");

	for (i = 0; decl->u.choice.choice[i].name; ++i) {
		if (i)
			printf("|");
		printf("%s", decl->u.choice.choice[i].name);
	}

	printf("\n");
}

static void print_bool_help(struct isl_arg *decl, const char *prefix)
{
	print_arg_help(decl, prefix);
	printf("\n");
}

static void print_help(struct isl_arg *arg, const char *prefix)
{
	int i;

	for (i = 0; arg[i].type != isl_arg_end; ++i) {
		switch (arg[i].type) {
		case isl_arg_choice:
			print_choice_help(&arg[i], prefix);
			break;
		case isl_arg_bool:
			print_bool_help(&arg[i], prefix);
			break;
		}
	}

	for (i = 0; arg[i].type != isl_arg_end; ++i) {
		if (arg[i].type != isl_arg_child)
			continue;

		printf("\n");
		print_help(arg[i].u.child.child, arg[i].long_name);
	}
}

static void print_help_and_exit(struct isl_arg *arg, const char *prog)
{
	const char *slash;

	slash = strrchr(prog, '/');
	if (slash)
		printf("Usage: %s [OPTION...]\n\n", slash + 1);

	print_help(arg, NULL);

	exit(0);
}

static int parse_choice_option(struct isl_arg *decl, const char *arg,
	const char *prefix, void *opt)
{
	int i;
	const char *equal;
	const char *name;

	if (strncmp(arg, "--", 2))
		return 0;

	name = arg + 2;
	equal = strchr(name, '=');
	if (!equal)
		return 0;

	if (prefix) {
		size_t prefix_len = strlen(prefix);
		if (strncmp(name, prefix, prefix_len) == 0 &&
		    name[prefix_len] == '-')
			name += prefix_len + 1;
	}

	if (strncmp(name, decl->long_name, equal - name))
		return 0;

	for (i = 0; decl->u.choice.choice[i].name; ++i) {
		if (strcmp(equal + 1, decl->u.choice.choice[i].name))
			continue;

		*(unsigned *)(((char *)opt) + decl->offset) =
			decl->u.choice.choice[i].value;

		return 1;
	}

	return 0;
}

static int parse_bool_option(struct isl_arg *decl, const char *arg, void *opt)
{
	int i;

	if ((arg[0] == '-' && arg[1] == decl->short_name && arg[2] == '\0') ||
	    (strncmp(arg, "--", 2) == 0 &&
	     strcmp(arg + 2, decl->long_name) == 0)) {
		*(unsigned *)(((char *)opt) + decl->offset) = 1;

		return 1;
	}

	return 0;
}

static int parse_option(struct isl_arg *decl, const char *arg,
	const char *prefix, void *opt);

static int parse_child_option(struct isl_arg *decl, const char *arg, void *opt)
{
	return parse_option(decl->u.child.child, arg, decl->long_name,
				*(void **)(((char *)opt) + decl->offset));
}

static int parse_option(struct isl_arg *decl, const char *arg,
	const char *prefix, void *opt)
{
	int i;

	for (i = 0; decl[i].type != isl_arg_end; ++i) {
		switch (decl[i].type) {
		case isl_arg_choice:
			if (parse_choice_option(&decl[i], arg, prefix, opt))
				return 1;
			break;
		case isl_arg_bool:
			if (parse_bool_option(&decl[i], arg, opt))
				return 1;
			break;
		case isl_arg_child:
			if (parse_child_option(&decl[i], arg, opt))
				return 1;
			break;
		}
	}

	return 0;
}

static int drop_argument(int argc, char **argv, int drop)
{
	for (; drop < argc; ++drop)
		argv[drop] = argv[drop + 1];

	return argc - 1;
}

int isl_arg_parse(struct isl_arg *arg, int argc, char **argv, void *opt)
{
	int skip = 0;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0)
			print_help_and_exit(arg, argv[0]);
	}

	while (argc > 1 + skip) {
		if (parse_option(arg, argv[1 + skip], NULL, opt))
			argc = drop_argument(argc, argv, 1 + skip);
		else {
			fprintf(stderr, "unrecognized option: %s\n",
					argv[1 + skip]);
			exit(-1);
		}
	}

	return argc;
}
