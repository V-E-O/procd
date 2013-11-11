#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include <arpa/inet.h>
#include <netinet/ether.h>
#include <sys/stat.h>

#include "libvalidate.h"

enum dt_optype {
	OP_UNKNOWN,
	OP_BOOL,
	OP_NUMBER,
	OP_STRING,
	OP_FUNCTION
};

struct dt_fun;

struct dt_op {
	enum dt_optype type;
	const char *next;
	int length;
	int nextop;
	union {
		bool boolean;
		double number;
		const char *string;
		struct dt_fun *function;
	} value;
};

struct dt_state {
	int pos;
	int depth;
	const char *value;
	struct dt_op stack[32];
};

struct dt_fun {
	const char *name;
	bool (*call)(struct dt_state *s, int nargs);
};

static bool
dt_test_number(double number, const char *value)
{
	char *e;
	double n;

	n = strtod(value, &e);

	return (e > value && *e == 0 && n == number);
}

static bool
dt_test_string(const char *s, const char *end, const char *value)
{
	bool esc = false;

	while (*value)
	{
		if (s > end)
			return false;

		if (!esc && *s == '\\')
		{
			s++;

			if (s >= end)
				break;

			esc = true;
			continue;
		}

		if (*s != *value)
			return false;

		esc = false;
		value++;
		s++;
	}

	return (*s == *value || (s > end && *value == 0));
}

static bool
dt_step(struct dt_state *s);

static bool
dt_call(struct dt_state *s);

static bool
dt_type_or(struct dt_state *s, int nargs)
{
	while (nargs--)
		if (dt_step(s))
			return true;

	return false;
}

static bool
dt_type_and(struct dt_state *s, int nargs)
{
	while (nargs--)
		if (!dt_step(s))
			return false;

	return true;
}

static bool
dt_type_not(struct dt_state *s, int nargs)
{
	if (!nargs)
		return false;

	return !dt_step(s);
}

static bool
dt_type_neg(struct dt_state *s, int nargs)
{
	bool rv;
	const char *value = s->value;

	if (!nargs)
		return false;

	if (*s->value == '!')
		while (isspace(*++s->value));

	rv = dt_step(s);
	s->value = value;

	return rv;
}

static bool
dt_type_list(struct dt_state *s, int nargs)
{
	bool rv = true;
	int pos = s->pos;
	char *p, *str = strdup(s->value);
	const char *value = s->value;

	if (!str || !nargs)
		return false;

	for (p = strtok(str, " \t"); p; p = strtok(NULL, " \t"))
	{
		s->value = p;

		if (!dt_step(s))
		{
			rv = false;
			break;
		}

		s->pos = pos;
	}

	s->value = value;
	free(str);

	return rv;
}

static bool
dt_type_min(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	if (nargs >= 1 && s->stack[s->pos].type == OP_NUMBER)
	{
		n = strtol(s->value, &e, 0);

		return (e > s->value && *e == 0 &&
		        n >= s->stack[s->pos].value.number);
	}

	return false;
}

static bool
dt_type_max(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	if (nargs >= 1 && s->stack[s->pos].type == OP_NUMBER)
	{
		n = strtol(s->value, &e, 0);

		return (e > s->value && *e == 0 &&
		        n <= s->stack[s->pos].value.number);
	}

	return false;
}

static bool
dt_type_range(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	if (nargs >= 2 &&
	    s->stack[s->pos].type == OP_NUMBER &&
	    s->stack[s->pos + 1].type == OP_NUMBER)
	{
		n = strtol(s->value, &e, 0);

		return (e > s->value && *e == 0 &&
		        n >= s->stack[s->pos].value.number &&
		        n <= s->stack[s->pos + 1].value.number);
	}

	return false;
}

static bool
dt_type_minlen(struct dt_state *s, int nargs)
{
	if (nargs >= 1 && s->stack[s->pos].type == OP_NUMBER)
		return (strlen(s->value) >= s->stack[s->pos].value.number);

	return false;
}

static bool
dt_type_maxlen(struct dt_state *s, int nargs)
{
	if (nargs >= 1 && s->stack[s->pos].type == OP_NUMBER)
		return (strlen(s->value) <= s->stack[s->pos].value.number);

	return false;
}

static bool
dt_type_rangelen(struct dt_state *s, int nargs)
{
	if (nargs >= 2 &&
	    s->stack[s->pos].type == OP_NUMBER &&
	    s->stack[s->pos + 1].type == OP_NUMBER)
		return (strlen(s->value) >= s->stack[s->pos].value.number &&
				strlen(s->value) <= s->stack[s->pos + 1].value.number);

	return false;
}

static bool
dt_type_int(struct dt_state *s, int nargs)
{
	char *e;

	strtol(s->value, &e, 0);

	return (e > s->value && *e == 0);
}

static bool
dt_type_uint(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	n = strtol(s->value, &e, 0);

	return (e > s->value && *e == 0 && n >= 0);
}

static bool
dt_type_float(struct dt_state *s, int nargs)
{
	char *e;

	strtod(s->value, &e);

	return (e > s->value && *e == 0);
}

static bool
dt_type_ufloat(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	n = strtod(s->value, &e);

	return (e > s->value && *e == 0 && n >= 0.0);
}

static bool
dt_type_bool(struct dt_state *s, int nargs)
{
	int i;
	const char *values[] = {
		"0", "off", "false", "no",
		"1", "on", "true", "yes"
	};

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
		if (!strcasecmp(values[i], s->value))
			return true;

	return false;
}

static bool
dt_type_string(struct dt_state *s, int nargs)
{
	return true;
}

static bool
dt_type_ip4addr(struct dt_state *s, int nargs)
{
	struct in6_addr a;
	return inet_pton(AF_INET, s->value, &a);
}

static bool
dt_type_ip6addr(struct dt_state *s, int nargs)
{
	struct in6_addr a;
	return inet_pton(AF_INET6, s->value, &a);
}

static bool
dt_type_ipaddr(struct dt_state *s, int nargs)
{
	return (dt_type_ip4addr(s, 0) || dt_type_ip6addr(s, 0));
}

static bool
dt_type_netmask4(struct dt_state *s, int nargs)
{
	int i;
	struct in_addr a;

	if (!inet_pton(AF_INET, s->value, &a))
		return false;

	if (a.s_addr == 0)
		return true;

	a.s_addr = ntohl(a.s_addr);

	for (i = 0; (i < 32) && !(a.s_addr & (1 << i)); i++);

	return ((uint32_t)(~((1 << i) - 1)) == a.s_addr);
}

static bool
dt_type_netmask6(struct dt_state *s, int nargs)
{
	int i;
	struct in6_addr a;

	if (!inet_pton(AF_INET6, s->value, &a))
		return false;

	for (i = 0; (i < 16) && (a.s6_addr[i] == 0xFF); i++);

	if (i == 16)
		return true;

	if ((a.s6_addr[i] != 255) && (a.s6_addr[i] != 254) &&
		(a.s6_addr[i] != 252) && (a.s6_addr[i] != 248) &&
		(a.s6_addr[i] != 240) && (a.s6_addr[i] != 224) &&
		(a.s6_addr[i] != 192) && (a.s6_addr[i] != 128) &&
		(a.s6_addr[i] != 0))
		return false;

	for (; (i < 16) && (a.s6_addr[i] == 0); i++);

	return (i == 16);
}

static bool
dt_type_cidr4(struct dt_state *s, int nargs)
{
	int n;
	struct in_addr a;
	char *p, buf[sizeof("255.255.255.255/32\0")];

	if (strlen(s->value) >= sizeof(buf))
		return false;

	strcpy(buf, s->value);
	p = strchr(buf, '/');

	if (p)
	{
		*p++ = 0;

		n = strtoul(p, &p, 10);

		if ((*p != 0) || (n > 32))
			return false;
	}

	return inet_pton(AF_INET, buf, &a);
}

static bool
dt_type_cidr6(struct dt_state *s, int nargs)
{
	int n;
	struct in6_addr a;
	char *p, buf[sizeof("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255/128\0")];

	if (strlen(s->value) >= sizeof(buf))
		return false;

	strcpy(buf, s->value);
	p = strchr(buf, '/');

	if (p)
	{
		*p++ = 0;

		n = strtoul(p, &p, 10);

		if ((*p != 0) || (n > 128))
			return false;
	}

	return inet_pton(AF_INET6, buf, &a);
}

static bool
dt_type_cidr(struct dt_state *s, int nargs)
{
	return (dt_type_cidr4(s, 0) || dt_type_cidr6(s, 0));
}

static bool
dt_type_ipmask4(struct dt_state *s, int nargs)
{
	bool rv;
	struct in_addr a;
	const char *value;
	char *p, buf[sizeof("255.255.255.255/255.255.255.255\0")];

	if (strlen(s->value) >= sizeof(buf))
		return false;

	strcpy(buf, s->value);
	p = strchr(buf, '/');

	if (p)
	{
		*p++ = 0;

		value = s->value;
		s->value = p;
		rv = dt_type_netmask4(s, 0);
		s->value = value;

		if (!rv)
			return false;
	}

	return inet_pton(AF_INET, buf, &a);
}

static bool
dt_type_ipmask6(struct dt_state *s, int nargs)
{
	bool rv;
	struct in6_addr a;
	const char *value;
	char *p, buf[sizeof("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255/"
	                    "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:255.255.255.255\0")];

	if (strlen(s->value) >= sizeof(buf))
		return false;

	strcpy(buf, s->value);
	p = strchr(buf, '/');

	if (p)
	{
		*p++ = 0;

		value = s->value;
		s->value = p;
		rv = dt_type_netmask6(s, 0);
		s->value = value;

		if (!rv)
			return false;
	}

	return inet_pton(AF_INET6, buf, &a);
}

static bool
dt_type_ipmask(struct dt_state *s, int nargs)
{
	return (dt_type_ipmask4(s, 0) || dt_type_ipmask6(s, 0));
}

static bool
dt_type_port(struct dt_state *s, int nargs)
{
	int n;
	char *e;

	n = strtoul(s->value, &e, 10);

	return (e > s->value && *e == 0 && n <= 65535);
}

static bool
dt_type_portrange(struct dt_state *s, int nargs)
{
	int n, m;
	char *e;

	n = strtoul(s->value, &e, 10);

	if (e == s->value || *e != '-')
		return false;

	m = strtoul(e + 1, &e, 10);

	return (*e == 0 && n <= 65535 && m <= 65535 && n <= m);
}

static bool
dt_type_macaddr(struct dt_state *s, int nargs)
{
	return !!ether_aton(s->value);
}

static bool
dt_type_uciname(struct dt_state *s, int nargs)
{
	const char *p;

	for (p = s->value;
	     *p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
	            (*p >= '0' && *p <= '9') || (*p == '_'));
		 p++);

	return (*p == 0);
}

static bool
dt_type_wpakey(struct dt_state *s, int nargs)
{
	int len = strlen(s->value);
	const char *p = s->value;

	if (len == 64)
	{
		while (isxdigit(*p))
			p++;

		return (*p == 0);
	}

	return (len >= 8 && len <= 63);
}

static bool
dt_type_wepkey(struct dt_state *s, int nargs)
{
	int len = strlen(s->value);
	const char *p = s->value;

	if (!strncmp(p, "s:", 2))
	{
		len -= 2;
		p += 2;
	}

	if (len == 10 || len == 26)
	{
		while (isxdigit(*p))
			p++;

		return (*p == 0);
	}

	return (len == 5 || len == 13);
}

static bool
dt_type_hostname(struct dt_state *s, int nargs)
{
	const char *p, *last;

	for (p = last = s->value; *p; p++)
	{
		if (*p == '.')
		{
			if ((p - last) == 0 || (p - last) > 63)
				return false;

			last = p + 1;
			continue;
		}
		else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		         (*p >= '0' && *p <= '9') || (*p == '_') || (*p == '-'))
		{
			continue;
		}

		return false;
	}

	return ((p - last) > 0 && (p - last) <= 255);
}

static bool
dt_type_host(struct dt_state *s, int nargs)
{
	return (dt_type_hostname(s, 0) || dt_type_ipaddr(s, 0));
}

static bool
dt_type_network(struct dt_state *s, int nargs)
{
	return (dt_type_uciname(s, 0) || dt_type_host(s, 0));
}

static bool
dt_type_phonedigit(struct dt_state *s, int nargs)
{
	const char *p;

	for (p = s->value;
	     *p && ((*p >= '0' && *p <= '9') || (*p == '*') || (*p == '#') ||
	            (*p == '!') || (*p == '.'));
		 p++);

	return (*p == 0);
}

static bool
dt_type_directory(struct dt_state *s, int nargs)
{
	struct stat st;
	return (!stat(s->value, &st) && S_ISDIR(st.st_mode));
}


static bool
dt_type_device(struct dt_state *s, int nargs)
{
	struct stat st;
	return (!stat(s->value, &st) &&
	        (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)));
}

static bool
dt_type_file(struct dt_state *s, int nargs)
{
	struct stat st;
	return (!stat(s->value, &st) && S_ISREG(st.st_mode));
}


static struct dt_fun dt_types[] = {
	{ "or",				dt_type_or			},
	{ "and",			dt_type_and			},
	{ "not",			dt_type_not			},
	{ "neg",			dt_type_neg			},
	{ "list",			dt_type_list		},
	{ "min",			dt_type_min			},
	{ "max",			dt_type_max			},
	{ "range",			dt_type_range		},
	{ "minlength",		dt_type_minlen		},
	{ "maxlength",		dt_type_maxlen		},
	{ "rangelength",	dt_type_rangelen	},
	{ "integer",		dt_type_int			},
	{ "uinteger",		dt_type_uint		},
	{ "float",			dt_type_float		},
	{ "ufloat",			dt_type_ufloat		},
	{ "bool",			dt_type_bool		},
	{ "string",			dt_type_string		},
	{ "ip4addr",		dt_type_ip4addr		},
	{ "ip6addr",		dt_type_ip6addr		},
	{ "ipaddr",			dt_type_ipaddr		},
	{ "cidr4",			dt_type_cidr4		},
	{ "cidr6",			dt_type_cidr6		},
	{ "cidr",			dt_type_cidr		},
	{ "netmask4",		dt_type_netmask4	},
	{ "netmask6",		dt_type_netmask6	},
	{ "ipmask4",		dt_type_ipmask4		},
	{ "ipmask6",		dt_type_ipmask6		},
	{ "ipmask",			dt_type_ipmask		},
	{ "port",			dt_type_port		},
	{ "portrange",		dt_type_portrange	},
	{ "macaddr",		dt_type_macaddr		},
	{ "uciname",        dt_type_uciname		},
	{ "wpakey",			dt_type_wpakey		},
	{ "wepkey",			dt_type_wepkey		},
	{ "hostname",		dt_type_hostname	},
	{ "host",			dt_type_host		},
	{ "network",		dt_type_network		},
	{ "phonedigit",		dt_type_phonedigit	},
	{ "directory",		dt_type_directory	},
	{ "device",			dt_type_device		},
	{ "file",			dt_type_file		},

	{ }
};

static struct dt_fun *
dt_lookup_function(const char *s, const char *e)
{
	struct dt_fun *fun = dt_types;

	while (fun->name)
	{
		if (!strncmp(fun->name, s, e - s) && *(fun->name + (e - s)) == '\0')
			return fun;

		fun++;
	}

	return NULL;
}

static bool
dt_parse_atom(struct dt_state *s, const char *label, const char *end)
{
	char q, *e;
	const char *p;
	bool esc;
	double dval;
	struct dt_fun *func;
	struct dt_op *op = &s->stack[s->depth];

	if ((s->depth + 1) >= (sizeof(s->stack) / sizeof(s->stack[0])))
	{
		printf("Syntax error, expression too long\n");
		return false;
	}

	while (isspace(*label))
		label++;

	/* test whether label is a float */
	dval = strtod(label, &e);

	if (e > label)
	{
		op->next = e;
		op->type = OP_NUMBER;
		op->value.number = dval;
		op->nextop = ++s->depth;

		return true;
	}
	else if ((*label == '"') || (*label == '\''))
	{
		for (p = label + 1, q = *label, esc = false; p <= end; p++)
		{
			if (esc)
			{
				esc = false;
				continue;
			}
			else if (*p == '\\')
			{
				esc = true;
				continue;
			}
			else if (*p == q)
			{
				op->next = p + 1;
				op->type = OP_STRING;
				op->length = (p - label) - 2;
				op->value.string = label + 1;
				op->nextop = ++s->depth;

				return true;
			}
		}

		printf("Syntax error, unterminated string\n");
		return false;
	}
	else if (*label)
	{
		for (p = label;
		     p <= end && ((*p >= 'A' && *p <= 'Z') ||
		                  (*p >= 'a' && *p <= 'z') ||
		                  (*p >= '0' && *p <= '9') ||
		                  (*p == '_'));
		     p++);

		func = dt_lookup_function(label, p);

		if (!func)
		{
			printf("Syntax error, unrecognized function\n");
			return false;
		}

		op->next = p;
		op->type = OP_FUNCTION;
		op->value.function = func;
		op->nextop = ++s->depth;

		return true;
	}

	printf("Syntax error, unexpected EOF\n");
	return false;
}

static bool
dt_parse_list(struct dt_state *s, const char *code, const char *end);

static bool
dt_parse_expr(const char *code, const char *end, struct dt_state *s)
{
	struct dt_op *tok;

	if (!dt_parse_atom(s, code, end))
		return false;

	tok = &s->stack[s->depth - 1];

	while (isspace(*tok->next))
		tok->next++;

	if (tok->type == OP_FUNCTION)
	{
		if (*tok->next == '(')
		{
			end--;

			while (isspace(*end) && end > tok->next + 1)
				end--;

			return dt_parse_list(s, tok->next + 1, end);
		}
		else if (tok->next == end)
		{
			return dt_parse_list(s, tok->next, tok->next);
		}

		printf("Syntax error, expected '(' or EOF after function label\n");
		return false;
	}
	else if (tok->next == end)
	{
		return true;
	}

	printf("Syntax error, expected ',' after literal\n");
	return false;
}

static bool
dt_parse_list(struct dt_state *s, const char *code, const char *end)
{
	char c;
	bool esc;
	int nest;
	const char *p, *last;
	struct dt_op *fptr;

	if (!code)
		return false;

	fptr = &s->stack[s->depth - 1];

	for (nest = 0, p = last = code, esc = false, c = *p;
	     p <= end;
	     p++, c = (p < end) ? *p : '\0')
	{
		if (esc)
		{
			esc = false;
			continue;
		}

		switch (c)
		{
		case '\\':
			esc = true;
			break;

		case '(':
			nest++;
			break;

		case ')':
			nest--;
			break;

		case ',':
		case '\0':
			if (nest <= 0)
			{
				if (p > last)
				{
					if (!dt_parse_expr(last, p, s))
						return false;

					fptr->length++;
				}

				last = p + 1;
			}

			break;
		}
	}

	fptr->nextop = s->depth;
	return true;
}

static bool
dt_step(struct dt_state *s)
{
	bool rv;
	struct dt_op *op = &s->stack[s->pos];

	switch (op->type)
	{
	case OP_BOOL:
		rv = op->value.boolean;
		break;

	case OP_NUMBER:
		rv = dt_test_number(op->value.number, s->value);
		break;

	case OP_STRING:
		rv = dt_test_string(op->value.string, op->value.string + op->length, s->value);
		break;

	case OP_FUNCTION:
		rv = dt_call(s);
		break;

	default:
		rv = false;
		break;
	}

	s->pos = op->nextop;
	return rv;
}

static bool
dt_call(struct dt_state *s)
{
	bool rv;
	struct dt_op *fptr = &s->stack[s->pos];
	struct dt_fun *func = fptr->value.function;

	s->pos++;

	rv = func->call(s, fptr->length);

	s->pos = fptr->nextop;

	return rv;
}

bool
dt_parse(const char *code, const char *value)
{
	struct dt_state s = {
		.depth = 1,
		.stack = {
			{
				.type = OP_FUNCTION,
				.value.function = &dt_types[0],
				.next = code
			}
		}
	};

	if (!value || !*value)
		return false;

	if (!dt_parse_list(&s, code, code + strlen(code)))
		return false;

	s.value = value;

	return dt_call(&s);
}