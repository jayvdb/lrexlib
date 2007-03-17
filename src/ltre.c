/* ltre.c - Lua binding of TRE regular expressions library */
/* See Copyright Notice in the file LICENSE */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lua.h"
#include "lauxlib.h"
#include "common.h"

#include <tre/regex.h>

/* These 2 settings may be redefined from the command-line or the makefile.
 * They should be kept in sync between themselves and with the target name.
 */
#ifndef REX_LIBNAME
#  define REX_LIBNAME "rex_tre"
#endif
#ifndef REX_OPENLIB
#  define REX_OPENLIB luaopen_rex_tre
#endif

#define CFLAGS_DEFAULT REG_EXTENDED
#define EFLAGS_DEFAULT 0

#define CODE_NOMATCH    REG_NOMATCH
#define IS_MATCH(res)   ((res) == 0)
#define SUB_BEG(ud,n)   ud->match[n].rm_so
#define SUB_END(ud,n)   ud->match[n].rm_eo
#define SUB_LEN(ud,n)   (SUB_END(ud,n) - SUB_BEG(ud,n))
#define SUB_VALID(ud,n) (SUB_BEG(ud,n) >= 0)
#define NSUB(ud)        ((int)ud->r.re_nsub)

#define PUSH_SUB(L,ud,text,n) \
  lua_pushlstring (L, (text) + SUB_BEG(ud,n), SUB_LEN(ud,n))

#define PUSH_SUB_OR_FALSE(L,ud,text,n) \
  (SUB_VALID(ud,n) ? PUSH_SUB (L,ud,text,n) : lua_pushboolean (L,0))

#define PUSH_START(L,ud,offs,n)   lua_pushinteger(L, (offs) + SUB_BEG(ud,n) + 1)
#define PUSH_END(L,ud,offs,n)     lua_pushinteger(L, (offs) + SUB_END(ud,n))
#define PUSH_OFFSETS(L,ud,offs,n) (PUSH_START(L,ud,offs,n), PUSH_END(L,ud,offs,n))

#define BASE(st)                        (st)
#define PULL(st,from)                   ((void)st)
#define OPTLOCALE(trg,L,pos)            ((void)trg)
#define DO_NAMED_SUBPATTERNS(L,ud,text) ((void)L)

typedef struct {
  regex_t      r;
  regmatch_t * match;
  int          freed;
} TPosix;

#define TUserdata TPosix
#include "algo.h"

const char posix_typename[] = REX_LIBNAME"_regex";
const char *posix_handle = posix_typename;

/*  Functions
 ******************************************************************************
 */

/* the table must be on Lua stack top */
static int get_int_field (lua_State *L, const char* field)
{
  int val;
  lua_getfield (L, -1, field);
  val = lua_tointeger (L, -1);
  lua_pop (L, 1);
  return val;
}

/* the table must be on Lua stack top */
static void set_int_field (lua_State *L, const char* field, int val)
{
  lua_pushinteger (L, val);
  lua_setfield (L, -2, field);
}

static void checkarg_regaparams (lua_State *L, int stackpos,  regaparams_t *argP) {
  if (lua_type (L, stackpos) != LUA_TTABLE) /* allow for userdata? */
    luaL_argerror (L, stackpos, "table expected");
  lua_pushvalue (L, stackpos);
  argP->cost_ins   = get_int_field (L, "cost_ins");
  argP->cost_del   = get_int_field (L, "cost_del");
  argP->cost_subst = get_int_field (L, "cost_subst");
  argP->max_cost   = get_int_field (L, "max_cost");
  argP->max_ins    = get_int_field (L, "max_ins");
  argP->max_del    = get_int_field (L, "max_del");
  argP->max_subst  = get_int_field (L, "max_subst");
  argP->max_err    = get_int_field (L, "max_err");
  lua_pop (L, 1);
}

static TPosix* check_ud (lua_State *L, int stackpos) {
  return (TPosix *)luaL_checkudata (L, stackpos, posix_handle);
}

/* method r:atfind (s, params, [st], [ef]) */
/* method r:aexec  (s, params, [st], [ef]) */
static void checkarg_atfind (lua_State *L, TArgExec *argE, TPosix **ud,
                             regaparams_t *argP) {
  *ud = check_ud (L, 1);
  argE->text = luaL_checklstring (L, 2, &argE->textlen);
  checkarg_regaparams (L, 3, argP);
  argE->startoffset = get_startoffset (L, 4, argE->textlen);
  argE->eflags = luaL_optint (L, 5, EFLAGS_DEFAULT);
}

static int generate_error (lua_State *L, const TPosix *ud, int errcode) {
  char errbuf[80];
  regerror (errcode, &ud->r, errbuf, sizeof (errbuf));
  return luaL_error (L, "%s", errbuf);
}

static int compile_regex (lua_State *L, const TArgComp *argC, TPosix **pud) {
  int res;
  TPosix *ud;

  ud = (TPosix *)lua_newuserdata (L, sizeof (TPosix));
  memset (ud, 0, sizeof (TPosix));          /* initialize all members to 0 */

  res = regncomp (&ud->r, argC->pattern, argC->patlen, argC->cflags);
  if (res != 0)
    return generate_error (L, ud, res);

  if (argC->cflags & REG_NOSUB)
    ud->r.re_nsub = 0;
  ud->match = (regmatch_t *) Lmalloc (L, (NSUB(ud) + 1) * sizeof (regmatch_t));
  luaL_getmetatable (L, posix_handle);
  lua_setmetatable (L, -2);

  if (pud) *pud = ud;
  return 1;
}

static int tfind_exec (TPosix *ud, TArgExec *argE) {
  argE->text += argE->startoffset;
  return regnexec (&ud->r, argE->text, argE->textlen - argE->startoffset,
                   NSUB(ud) + 1, ud->match, argE->eflags);
}

static int generic_atfind (lua_State *L, int tfind) {
  int res;
  TArgExec argE;
  TPosix *ud;
  regaparams_t argP;
  regamatch_t res_match;

  checkarg_atfind (L, &argE, &ud, &argP);
  if (argE.startoffset > (int)argE.textlen)
    return lua_pushnil(L), 1;

  argE.text += argE.startoffset;
  res_match.nmatch = NSUB(ud) + 1;
  res_match.pmatch = ud->match;

  /* execute the search */
  res = reganexec (&ud->r, argE.text, argE.textlen - argE.startoffset,
                   &res_match, argP, argE.eflags);
  if (IS_MATCH (res)) {
    PUSH_OFFSETS (L, ud, argE.startoffset, 0);
    if (tfind)
      push_substring_table (L, ud, argE.text);
    else
      push_offset_table (L, ud, argE.startoffset);
    /* set values in the dictionary part of the table */
    set_int_field (L, "cost", res_match.cost);
    set_int_field (L, "num_ins", res_match.num_ins);
    set_int_field (L, "num_del", res_match.num_del);
    set_int_field (L, "num_subst", res_match.num_subst);
    return 3;
  }
  else if (res == CODE_NOMATCH)
    return lua_pushnil (L), 1;
  else
    return generate_error (L, ud, res);
}

static int Ltre_atfind (lua_State *L) {
  return generic_atfind (L, 1);
}

static int Ltre_aexec (lua_State *L) {
  return generic_atfind (L, 0);
}

static int gmatch_exec (TUserdata *ud, TArgExec *argE) {
  if (argE->startoffset > 0)
    argE->eflags |= REG_NOTBOL;
  argE->text += argE->startoffset;
  return regnexec (&ud->r, argE->text, argE->textlen - argE->startoffset,
                   NSUB(ud) + 1, ud->match, argE->eflags);
}

static void gmatch_pushsubject (lua_State *L, TArgExec *argE) {
  lua_pushlstring (L, argE->text, argE->textlen);
}

static int findmatch_exec (TPosix *ud, TArgExec *argE) {
  argE->text += argE->startoffset;
  return regnexec (&ud->r, argE->text, argE->textlen - argE->startoffset,
                   NSUB(ud) + 1, ud->match, argE->eflags);
}

static int gsub_exec (TPosix *ud, TArgExec *argE, int st) {
  if (st > 0)
    argE->eflags |= REG_NOTBOL;
  return regnexec (&ud->r, argE->text+st, argE->textlen-st, NSUB(ud)+1,
                    ud->match, argE->eflags);
}

static int split_exec (TPosix *ud, TArgExec *argE, int offset) {
  if (offset > 0)
    argE->eflags |= REG_NOTBOL;
  return regnexec (&ud->r, argE->text + offset, argE->textlen - offset, NSUB(ud) + 1,
                   ud->match, argE->eflags);
}

static int Ltre_have_backrefs (lua_State *L) {
  TPosix *ud = check_ud (L, 1);
  lua_pushboolean (L, tre_have_backrefs (&ud->r));
  return 1;
}

static int Ltre_have_approx (lua_State *L) {
  TPosix *ud = check_ud (L, 1);
  lua_pushboolean (L, tre_have_approx (&ud->r));
  return 1;
}

static int Ltre_gc (lua_State *L) {
  TPosix *ud = check_ud (L, 1);
  if (ud->freed == 0) {           /* precaution against "manual" __gc calling */
    ud->freed = 1;
    regfree (&ud->r);
    if (ud->match)
      free (ud->match);
  }
  return 0;
}

static int Ltre_tostring (lua_State *L) {
  TPosix *ud = check_ud (L, 1);
  if (ud->freed == 0)
    lua_pushfstring (L, "%s (%p)", posix_typename, (void*)ud);
  else
    lua_pushfstring (L, "%s (deleted)", posix_typename);
  return 1;
}

static flag_pair tre_flags[] =
{
  { "BASIC",    REG_BASIC },
  { "NOSPEC",   REG_NOSPEC },
  { "EXTENDED", REG_EXTENDED },
  { "ICASE",    REG_ICASE },
  { "NOSUB",    REG_NOSUB },
  { "NEWLINE",  REG_NEWLINE },
  { "NOTBOL",   REG_NOTBOL },
  { "NOTEOL",   REG_NOTEOL },
  /* TRE-specific flags */
  { "LITERAL",              REG_LITERAL },
  { "RIGHT_ASSOC",          REG_RIGHT_ASSOC },
  { "UNGREEDY",             REG_UNGREEDY },
  { "APPROX_MATCHER",       REG_APPROX_MATCHER },
  { "BACKTRACKING_MATCHER", REG_BACKTRACKING_MATCHER },
/*---------------------------------------------------------------------------*/
  { NULL, 0 }
};

static flag_pair tre_error_flags[] = {
  { "OK",       REG_OK }, /* TRE-specific */
  { "NOMATCH",  REG_NOMATCH },
  { "BADPAT",   REG_BADPAT },
  { "ECOLLATE", REG_ECOLLATE },
  { "ECTYPE",   REG_ECTYPE },
  { "EESCAPE",  REG_EESCAPE },
  { "ESUBREG",  REG_ESUBREG },
  { "EBRACK",   REG_EBRACK },
  { "EPAREN",   REG_EPAREN },
  { "EBRACE",   REG_EBRACE },
  { "BADBR",    REG_BADBR },
  { "ERANGE",   REG_ERANGE },
  { "ESPACE",   REG_ESPACE },
  { "BADRPT",   REG_BADRPT },
/*---------------------------------------------------------------------------*/
  { NULL, 0 }
};

/* config. flags with integer value */
static flag_pair tre_config_flags_int[] = {
  { "CONFIG_APPROX",     TRE_CONFIG_APPROX },
  { "CONFIG_WCHAR",      TRE_CONFIG_WCHAR },
  { "CONFIG_MULTIBYTE",  TRE_CONFIG_MULTIBYTE },
  { "CONFIG_SYSTEM_ABI", TRE_CONFIG_SYSTEM_ABI },
  { NULL, 0 }
};

/* config. flags with string value */
static flag_pair tre_config_flags_str[] = {
  { "CONFIG_VERSION",    TRE_CONFIG_VERSION },
  { NULL, 0 }
};

static int Ltre_get_flags (lua_State *L) {
  const flag_pair* fps[] = { tre_flags, tre_error_flags, NULL };
  return get_flags (L, fps);
}

static int Ltre_config (lua_State *L) {
  int intval;
  const char *strval;
  flag_pair *fp;
  if (lua_istable (L, 1))
    lua_settop (L, 1);
  else
    lua_newtable (L);
  for (fp = tre_config_flags_int; fp->key; ++fp) {
    if (0 == tre_config (fp->val, &intval)) {
      lua_pushinteger (L, intval);
      lua_setfield (L, -2, fp->key);
    }
  }
  for (fp = tre_config_flags_str; fp->key; ++fp) {
    if (0 == tre_config (fp->val, &strval)) {
      lua_pushstring (L, strval);
      lua_setfield (L, -2, fp->key);
    }
  }
  return 1;
}

static int Ltre_version (lua_State *L) {
  lua_pushstring (L, tre_version ());
  return 1;
}

static const luaL_reg posixmeta[] = {
  { "exec",          ud_exec },
  { "tfind",         ud_tfind },
  { "aexec",         Ltre_aexec },
  { "atfind",        Ltre_atfind },
  { "have_backrefs", Ltre_have_backrefs },
  { "have_approx",   Ltre_have_approx },
  { "__gc",          Ltre_gc },
  { "__tostring",    Ltre_tostring },
  { NULL, NULL}
};

static const luaL_reg rexlib[] = {
  { "match",      match },
  { "find",       find },
  { "gmatch",     gmatch },
  { "gsub",       gsub },
  { "split",      split },
  { "new",        ud_new },
  { "flags",      Ltre_get_flags },
  { "plainfind",  plainfind_func },
  { "config",     Ltre_config },
  { "version",    Ltre_version },
  { NULL, NULL }
};

/* Open the library */
REX_API int REX_OPENLIB (lua_State *L)
{
  createmeta (L, posix_handle);
  luaL_register (L, NULL, posixmeta);
  lua_pop (L, 1);
  luaL_register (L, REX_LIBNAME, rexlib);
  lua_pushliteral (L, REX_VERSION" (for TRE regexes)");
  lua_setfield (L, -2, "_VERSION");
  return 1;
}

