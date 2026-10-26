//Farid Hajiyev 251ADB132
// (optional) https://github.com/<farid520>/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: AI (Claude, Anthropic) - help with program design and the first draft; prompts and
//   Specification: the lab handout (grammar hint in section 8).

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// TODO: put YOUR data here (lower case, as in the lab examples).
// ---------------------------------------------------------------------------
#define STUDENT_NAME     "anna"
#define STUDENT_LASTNAME "berzina"
#define STUDENT_ID       "221234"

// ===========================================================================
// 1. Growable byte buffer
// ===========================================================================
// Used for the whole input file and for the collected results. Capacity doubles
// when full, so appending n bytes costs O(n) in total and nothing is ever
// copied more than a constant number of times. This is what keeps files of
// several megabytes fast.
// AI (Claude): prompt "how to read a whole file into memory in C without
//   knowing its size" -> the doubling idea; the code below is adapted by me.

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + extra) ncap *= 2;
    char *p = realloc(b->data, ncap);
    if (!p) return false;
    b->data = p;
    b->cap = ncap;
    return true;
}

static bool buf_append(Buf *b, const char *s, size_t n) {
    if (!buf_reserve(b, n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return true;
}

// Reads the whole file into b. Returns false (with errno set) on failure.
static bool read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    for (;;) {
        if (!buf_reserve(b, 65536)) { fclose(f); errno = ENOMEM; return false; }
        size_t got = fread(b->data + b->len, 1, b->cap - b->len, f);
        b->len += got;
        if (got == 0) break;  // EOF or error; ferror() tells which
    }
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// ===========================================================================
// 2. Numbers
// ===========================================================================
// A value is either an integer or a double. The flag travels with the value
// so every operator can decide at run time: if BOTH operands are ints the
// operation is done in long long (C semantics, e.g. 7/2 == 3); if EITHER is a
// float the other is converted and the operation is done in double.
typedef struct {
    bool      is_float;
    long long i;
    double    d;
} Num;

static double as_double(Num n) { return n.is_float ? n.d : (double)n.i; }

// ===========================================================================
// 3. Tokenizer
// ===========================================================================
typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_MUL, T_DIV, T_POW, T_LP, T_RP,
    T_EOL,  // end of the current line ('\n' or end of file)
    T_BAD   // a character that cannot start any token
} Kind;

typedef struct {
    Kind   kind;
    size_t start;  // 0-based offset in the WHOLE file buffer
    Num    val;    // only for T_NUM
} Token;

typedef struct {
    const char *src;   // whole file, never copied or split into lines
    size_t      len;
    size_t      p;     // lexer cursor
    Token       cur;   // one token of lookahead
    size_t      err;   // 0 = no error, otherwise the 1-BASED error position
    Buf         lexeme;  // scratch space for strtod, reused for all floats
} Parser;

// Position tracking: the lexer never resets its offset at a new line. `p` is
// an index into the one big buffer, so it already counts every character of
// the file (newlines and comment lines included). Because a token's `start`
// is that index, the required 1-based position is simply start + 1, with no
// per-line bookkeeping that could drift.

static void next_token(Parser *ps) {
    const char *s = ps->src;
    while (ps->p < ps->len && (s[ps->p] == ' ' || s[ps->p] == '\t')) ps->p++;

    Token *t = &ps->cur;
    t->start = ps->p;
    t->kind = T_BAD;

    // The '\n' is NOT consumed: the line loop in evaluate_text() skips it. At
    // end of file without '\n', p == len, so the position is file length + 1,
    // exactly what the spec asks for.
    if (ps->p >= ps->len || s[ps->p] == '\n') { t->kind = T_EOL; return; }

    char c = s[ps->p];
    switch (c) {
    case '+': t->kind = T_PLUS;  ps->p++; return;
    case '-': t->kind = T_MINUS; ps->p++; return;
    case '/': t->kind = T_DIV;   ps->p++; return;
    case '(': t->kind = T_LP;    ps->p++; return;
    case ')': t->kind = T_RP;    ps->p++; return;
    case '*':
        // Longest match: "**" must win over two separate '*'.
        if (ps->p + 1 < ps->len && s[ps->p + 1] == '*') { t->kind = T_POW; ps->p += 2; }
        else { t->kind = T_MUL; ps->p++; }
        return;
    default: break;
    }

    if (isdigit((unsigned char)c)) {
        size_t b = ps->p;
        unsigned long long v = 0;
        while (ps->p < ps->len && isdigit((unsigned char)s[ps->p])) {
            v = v * 10 + (unsigned)(s[ps->p] - '0');  // unsigned: no UB if it wraps
            ps->p++;
        }
        t->kind = T_NUM;
        t->val.is_float = false;
        t->val.i = (long long)v;
        // A '.' only makes this a float if a digit follows. Otherwise ("5." or
        // "5.x") we stop here and leave the '.' for the next call, which
        // reports it as T_BAD at the '.' - exactly where the spec wants it.
        // The same trick handles "1.5e3": the number ends before the 'e'.
        if (ps->p + 1 < ps->len && s[ps->p] == '.' && isdigit((unsigned char)s[ps->p + 1])) {
            ps->p++;
            while (ps->p < ps->len && isdigit((unsigned char)s[ps->p])) ps->p++;
            // strtod would also swallow an exponent like "e3", so I give it
            // only the exact lexeme, copied to a NUL-terminated scratch buffer.
            size_t n = ps->p - b;
            ps->lexeme.len = 0;
            if (!buf_append(&ps->lexeme, s + b, n) || !buf_append(&ps->lexeme, "", 1)) {
                fprintf(stderr, "calc: out of memory\n");
                exit(1);
            }
            t->val.is_float = true;
            t->val.d = strtod(ps->lexeme.data, NULL);
        }
        return;
    }

    // Anything else (letters, '#' in the middle of a line, '\r', ...) is an
    // invalid character. We do not report it yet; we hand the parser a token
    // that matches nothing, so the parser fails at its start - see fail().
    ps->p++;
}

// ===========================================================================
// 4. Errors and arithmetic
// ===========================================================================
// Only the FIRST error matters, so once err is set every parse function just
// returns false and nothing overwrites it.
static bool fail(Parser *ps, size_t offset0) {
    if (ps->err == 0) ps->err = offset0 + 1;  // convert 0-based to 1-based
    return false;
}

// Why errors are reported at these places:
//  * syntax errors (bad char, two operators, missing operand/')', extra ')'):
//    the parser always fails at the token it is currently looking at, so
//    callers use fail(ps, ps->cur.start). A bad character is such a token too,
//    which is why it needs no special case.
//  * division by zero / negative exponent: the problem is the right operand,
//    so we report where that operand started (rhs_start).
//  * infinite/NaN float result: nothing is wrong with an operand, the
//    operation itself overflowed, so we report the operator (op_start).
static bool apply(Parser *ps, Kind op, size_t op_start, size_t rhs_start,
                  Num a, Num b, Num *out) {
    if (!a.is_float && !b.is_float) {
        // Integer arithmetic. Done in unsigned so a wrap-around is defined
        // behaviour (the tests never overflow, this just keeps sanitizers calm).
        unsigned long long x = (unsigned long long)a.i, y = (unsigned long long)b.i;
        out->is_float = false;
        switch (op) {
        case T_PLUS:  out->i = (long long)(x + y); return true;
        case T_MINUS: out->i = (long long)(x - y); return true;
        case T_MUL:   out->i = (long long)(x * y); return true;
        case T_DIV:
            if (b.i == 0) return fail(ps, rhs_start);
            if (a.i == (-9223372036854775807LL - 1) && b.i == -1) {
                out->i = a.i;  // would overflow; avoid UB
                return true;
            }
            out->i = a.i / b.i;  // C truncates toward zero, as required
            return true;
        case T_POW: {
            if (b.i < 0) return fail(ps, rhs_start);
            // Exponentiation by squaring: O(log n) multiplications, so
            // "1 ** 1000000000" is instant. We only square while exponent
            // bits remain, to avoid needless overflow.
            unsigned long long r = 1, base = x, e = (unsigned long long)b.i;
            while (e) {
                if (e & 1) r *= base;
                e >>= 1;
                if (e) base *= base;
            }
            out->i = (long long)r;
            return true;
        }
        default: return false;
        }
    }

    // At least one float: int vs float is decided here, per operation, by the
    // flags on the two operands. Convert both to double and compute in double.
    double x = as_double(a), y = as_double(b), r = 0;
    switch (op) {
    case T_PLUS:  r = x + y; break;
    case T_MINUS: r = x - y; break;
    case T_MUL:   r = x * y; break;
    case T_DIV:
        if (y == 0.0) return fail(ps, rhs_start);
        r = x / y;
        break;
    case T_POW: r = pow(x, y); break;
    default: return false;
    }
    if (!isfinite(r)) return fail(ps, op_start);  // inf or NaN, e.g. (0-8) ** 0.5
    out->is_float = true;
    out->d = r;
    return true;
}

// ===========================================================================
// 5. Recursive descent parser (evaluates while parsing)
// ===========================================================================
// One function per precedence level, lowest binds weakest:
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
// Lower levels call higher levels for their operands, so "3 + 2 * 5" parses
// as 3 + (2*5): expr asks term for its right side, and term grabs the '*'
// first. Nothing is built as a tree; each function returns the value of what
// it just parsed, which is enough because we only need the result.

static bool parse_expr(Parser *ps, Num *out);

static bool parse_primary(Parser *ps, Num *out) {
    if (ps->cur.kind == T_NUM) {
        *out = ps->cur.val;
        next_token(ps);
        return true;
    }
    if (ps->cur.kind == T_LP) {
        next_token(ps);
        if (!parse_expr(ps, out)) return false;
        // Missing ')' shows up as T_EOL here (position = the '\n'); an
        // unexpected token of any other kind is reported at its own start.
        if (ps->cur.kind != T_RP) return fail(ps, ps->cur.start);
        next_token(ps);
        return true;
    }
    // Operator, ')', end of line, or bad character where an operand is
    // needed. This covers "7 + * 3", "3 *<EOL>", "2 * ()" and unary "-5".
    return fail(ps, ps->cur.start);
}

static bool parse_power(Parser *ps, Num *out) {
    Num left;
    if (!parse_primary(ps, &left)) return false;
    if (ps->cur.kind == T_POW) {
        size_t op_start = ps->cur.start;
        next_token(ps);
        size_t rhs_start = ps->cur.start;
        Num right;
        // Right associativity: the exponent is parsed by parse_power()
        // ITSELF, so in 2 ** 3 ** 2 the inner call consumes "3 ** 2" first
        // and we get 2 ** (3 ** 2). A loop (as in expr/term) would instead
        // combine left to right. Because ** is also parsed one level above
        // '*', 2 * 3 ** 2 is 2 * (3 ** 2).
        if (!parse_power(ps, &right)) return false;
        return apply(ps, T_POW, op_start, rhs_start, left, right, out);
    }
    *out = left;
    return true;
}

static bool parse_term(Parser *ps, Num *out) {
    Num acc;
    if (!parse_power(ps, &acc)) return false;
    // A loop (not recursion) keeps '*' and '/' LEFT associative:
    // 8 / 2 / 2 is (8/2)/2, because acc is folded before the next operand.
    while (ps->cur.kind == T_MUL || ps->cur.kind == T_DIV) {
        Kind op = ps->cur.kind;
        size_t op_start = ps->cur.start;
        next_token(ps);
        size_t rhs_start = ps->cur.start;
        Num rhs;
        if (!parse_power(ps, &rhs)) return false;
        if (!apply(ps, op, op_start, rhs_start, acc, rhs, &acc)) return false;
    }
    *out = acc;
    return true;
}

static bool parse_expr(Parser *ps, Num *out) {
    Num acc;
    if (!parse_term(ps, &acc)) return false;
    while (ps->cur.kind == T_PLUS || ps->cur.kind == T_MINUS) {
        Kind op = ps->cur.kind;
        size_t op_start = ps->cur.start;
        next_token(ps);
        size_t rhs_start = ps->cur.start;
        Num rhs;
        if (!parse_term(ps, &rhs)) return false;
        if (!apply(ps, op, op_start, rhs_start, acc, rhs, &acc)) return false;
    }
    *out = acc;
    return true;
}

// ===========================================================================
// 6. Whole-file evaluation
// ===========================================================================
// Appends one printed result and '\n' to the result buffer.
static bool append_result(Buf *out, Num v) {
    char tmp[64];
    int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                       : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    return n > 0 && buf_append(out, tmp, (size_t)n);
}

// Evaluates the whole text. On success `out` holds one result per line. On the
// first error `out` is replaced by just "ERROR:<pos>\n" - the spec forbids
// partial results, so we collect everything in memory first and decide at the
// end instead of writing line by line. Returns false only on out-of-memory.
static bool evaluate_text(const char *src, size_t len, Buf *out) {
    Parser ps = {0};
    ps.src = src;
    ps.len = len;
    bool ok = true;

    size_t p = 0;  // start of the current line; only ever moves forward
    while (p < len) {
        // Look at the first non-blank character to classify the line without
        // copying it anywhere.
        size_t q = p;
        while (q < len && (src[q] == ' ' || src[q] == '\t')) q++;

        if (q >= len) break;  // trailing blanks after the last '\n'
        if (src[q] == '\n') { p = q + 1; continue; }  // blank line: no output

        if (src[q] == '#') {
            // Comment line. It is skipped but still counted in positions,
            // simply because p keeps indexing the same buffer. memchr is a
            // fast scan even for very long lines.
            const char *nl = memchr(src + q, '\n', len - q);
            p = nl ? (size_t)(nl - src) + 1 : len;
            continue;
        }

        ps.p = p;
        ps.err = 0;
        next_token(&ps);  // load the first lookahead token
        Num v;
        bool good = parse_expr(&ps, &v);
        // The whole line must be consumed: anything left (e.g. the extra ')'
        // in "(1 + 2))" or a second number "1 2") is an error at that token.
        if (good && ps.cur.kind != T_EOL) good = fail(&ps, ps.cur.start);

        if (!good) {
            out->len = 0;
            char tmp[64];
            int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", ps.err);
            ok = n > 0 && buf_append(out, tmp, (size_t)n);
            break;  // first error ends everything
        }
        if (!append_result(out, v)) { ok = false; break; }

        // cur is T_EOL, so its start is the '\n' (or len). Skip past it.
        p = ps.cur.start + 1;
    }
    free(ps.lexeme.data);
    return ok;
}

// ===========================================================================
// 7. Files, folders and command line
// ===========================================================================
static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Creates `path` if missing (parent must exist). Existing folder is fine.
static bool ensure_dir(const char *path) {
    if (is_dir(path)) return true;
    if (mkdir(path, 0777) == 0) return true;
    return errno == EEXIST && is_dir(path);
}

// Small allocation helpers (no varargs needed).
static char *dup_range(const char *s, size_t n) {
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *join3(const char *a, const char *b, const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *r = malloc(la + lb + lc + 1);
    if (!r) return NULL;
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    memcpy(r + la + lb, c, lc);
    r[la + lb + lc] = '\0';
    return r;
}

// Returns a malloc'ed copy of the last path component, without trailing '/'
// and, if strip_txt is set, without a final ".txt".
static char *base_name(const char *path, bool strip_txt) {
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = end - start;
    if (strip_txt && n > 4 && strcmp(path + end - 4, ".txt") == 0) n -= 4;
    return dup_range(path + start, n);
}

// Evaluates one input file and writes <outdir>/<base>_<name>_<lastname>_<id>.txt
// Returns false (after printing a message) if it could not be done.
static bool process_file(const char *in_path, const char *outdir) {
    Buf in = {0}, out = {0};
    bool ok = false;
    char *base = NULL, *suffix = NULL, *file = NULL, *dir_slash = NULL, *full = NULL;

    if (!read_file(in_path, &in)) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        goto done;
    }
    if (!evaluate_text(in.data ? in.data : "", in.len, &out)) {
        fprintf(stderr, "calc: out of memory while evaluating '%s'\n", in_path);
        goto done;
    }

    base = base_name(in_path, true);
    suffix = join3("_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID, ".txt", "");
    dir_slash = join3(outdir, "/", "");
    if (!base || !suffix || !dir_slash) { fprintf(stderr, "calc: out of memory\n"); goto done; }
    file = join3(base, suffix, "");
    full = file ? join3(dir_slash, file, "") : NULL;
    if (!full) { fprintf(stderr, "calc: out of memory\n"); goto done; }

    FILE *f = fopen(full, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", full, strerror(errno));
        goto done;
    }
    // One fwrite for the entire result; fclose must be checked too, because
    // buffered data can fail to reach the disk only at that point.
    bool wrote = out.len == 0 || fwrite(out.data, 1, out.len, f) == out.len;
    if (fclose(f) != 0) wrote = false;
    if (!wrote) {
        fprintf(stderr, "calc: error writing '%s'\n", full);
        goto done;
    }
    ok = true;

done:
    free(in.data); free(out.data);
    free(base); free(suffix); free(file); free(dir_slash); free(full);
    return ok;
}

static bool ends_with_txt(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".txt") == 0;
}

// Evaluates every *.txt regular file directly inside dir (no recursion).
static bool process_dir(const char *dir, const char *outdir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return false;
    }
    bool all_ok = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with_txt(e->d_name)) continue;
        char *dir_slash = join3(dir, "/", "");
        char *path = dir_slash ? join3(dir_slash, e->d_name, "") : NULL;
        free(dir_slash);
        if (!path) { fprintf(stderr, "calc: out of memory\n"); all_ok = false; break; }
        // stat, not d_type: d_type is not available on every filesystem. A
        // folder called "x.txt" is skipped here.
        if (is_regular_file(path) && !process_file(path, outdir)) all_ok = false;
        free(path);
    }
    closedir(d);
    return all_ok;
}

static void usage(void) {
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *dir = NULL, *outdir = NULL, *input = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool is_d = strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0;
        bool is_o = strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0;
        if (is_d || is_o) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a value\n", a);
                usage();
                return 2;
            }
            if (is_d) dir = argv[++i]; else outdir = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 2;
        } else if (input) {
            fprintf(stderr, "calc: more than one input file given\n");
            usage();
            return 2;
        } else {
            input = a;
        }
    }

    if ((!dir && !input) || (dir && input)) {
        usage();  // no arguments, or ambiguous: need exactly one of -d / file
        return 2;
    }

    // Default output folder: <input_base>_<name>_<studentid>. With -d the base
    // is the folder's own name. "." and ".." are not usable names, so I fall
    // back to "output" for them.
    char *default_out = NULL;
    if (!outdir) {
        char *base = base_name(dir ? dir : input, dir == NULL);
        if (base && (strcmp(base, ".") == 0 || strcmp(base, "..") == 0 || base[0] == '\0')) {
            free(base);
            base = dup_range("output", 6);
        }
        default_out = base ? join3(base, "_" STUDENT_NAME "_" STUDENT_ID, "") : NULL;
        free(base);
        if (!default_out) { fprintf(stderr, "calc: out of memory\n"); return 1; }
        outdir = default_out;
    }

    // Check the input exists BEFORE creating the output folder, so a typo in
    // the path does not leave an empty folder behind.
    int rc = 0;
    if (dir ? !is_dir(dir) : !is_regular_file(input)) {
        fprintf(stderr, "calc: '%s' not found or not a %s\n",
                dir ? dir : input, dir ? "folder" : "file");
        rc = 1;
    } else if (!ensure_dir(outdir)) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n", outdir, strerror(errno));
        rc = 1;
    } else {
        bool ok = dir ? process_dir(dir, outdir) : process_file(input, outdir);
        rc = ok ? 0 : 1;
    }
    free(default_out);
    return rc;
}