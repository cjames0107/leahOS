/* Print, with control over exactly what comes out.
 *
 * echo adds a newline, cannot be persuaded to emit a tab, and has no way to
 * say "byte 27". This is the command that can, which is why every script that
 * wants to colour its output or draw a box reaches for it.
 *
 * The format is reused until the arguments run out, as it is everywhere -
 * `printf '%s\n' a b c` prints three lines - because that turns the common
 * case of "do this to each of these" into one command.
 */

#include <cli.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* \n, \t, \\, \0NNN and \xNN, in a string that may be either a format or an
 * argument to %b. Returns how many bytes were written. */
static unsigned long unescape(const char* in, char* out, unsigned long max)
{
    unsigned long n = 0;
    while (*in != '\0' && n + 1 < max) {
        if (*in != '\\') {
            out[n++] = *in++;
            continue;
        }
        ++in;
        switch (*in) {
        case 'n':  out[n++] = '\n'; ++in; break;
        case 't':  out[n++] = '\t'; ++in; break;
        case 'r':  out[n++] = '\r'; ++in; break;
        case 'a':  out[n++] = '\a'; ++in; break;
        case 'b':  out[n++] = '\b'; ++in; break;
        case 'f':  out[n++] = '\f'; ++in; break;
        case 'v':  out[n++] = '\v'; ++in; break;
        case 'e':  out[n++] = 0x1B; ++in; break;   /* not POSIX, universal */
        case '\\': out[n++] = '\\'; ++in; break;
        case '\0': out[n++] = '\\'; break;
        case 'x': {
            ++in;
            int value = 0, digits = 0;
            while (digits < 2) {
                int d;
                if (*in >= '0' && *in <= '9')      d = *in - '0';
                else if (*in >= 'a' && *in <= 'f') d = *in - 'a' + 10;
                else if (*in >= 'A' && *in <= 'F') d = *in - 'A' + 10;
                else break;
                value = value * 16 + d;
                ++in;
                ++digits;
            }
            out[n++] = digits > 0 ? (char)value : 'x';
            break;
        }
        default: {
            /* \0NNN and, because half the world writes it that way, \NNN. */
            if (*in == '0')
                ++in;
            int value = 0, digits = 0;
            while (digits < 3 && *in >= '0' && *in <= '7') {
                value = value * 8 + (*in++ - '0');
                ++digits;
            }
            if (digits > 0)
                out[n++] = (char)value;
            else
                out[n++] = *in != '\0' ? *in++ : '\\';
            break;
        }
        }
    }
    out[n] = '\0';
    return n;
}

/* One conversion, given its specifier and the argument it consumes. */
static void convert(const char* spec, char final, const char* argument)
{
    char format[32];
    const unsigned long len = strlen(spec);
    if (len + 2 >= sizeof(format)) {
        fputs(spec, stdout);
        return;
    }
    memcpy(format, spec, len);
    format[len] = final;
    format[len + 1] = '\0';

    switch (final) {
    case 'd': case 'i':
        printf(format, atoi_simple(argument));
        break;
    case 'u': case 'x': case 'X': case 'o':
        printf(format, (unsigned)atoi_simple(argument));
        break;
    case 'c':
        printf(format, argument[0]);
        break;
    case 'f': case 'e': case 'g': case 'E': case 'G':
        printf(format, strtod(argument, 0));
        break;
    default:
        printf(format, argument);
        break;
    }
}

int main(int argc, char** argv)
{
    /* The format is written by the caller and may well begin with a dash, so
     * the library parses nothing here. */
    cli_begin(argc, argv, "FORMAT [ARGUMENT...]", 0);
    if (argc < 2)
        cli_usage();

    char format[1024];
    unescape(argv[1], format, sizeof(format));

    int at = 2;
    for (;;) {
        const int started_at = at;

        for (const char* p = format; *p != '\0'; ) {
            if (*p != '%') {
                fputc(*p++, stdout);
                continue;
            }
            if (p[1] == '%') {
                fputc('%', stdout);
                p += 2;
                continue;
            }

            /* Copy the flags, width and precision through to printf, which
             * already knows what to do with them. */
            char spec[24];
            unsigned long n = 0;
            spec[n++] = *p++;
            while (*p != '\0' && n + 1 < sizeof(spec) &&
                   strchr("-+ #0123456789.", *p) != 0)
                spec[n++] = *p++;
            spec[n] = '\0';

            if (*p == '\0')
                break;
            const char final = *p++;
            const char* argument = at < argc ? argv[at++] : "";

            if (final == 'b') {
                /* %b is the argument with its escapes expanded - the one
                 * conversion printf(3) does not have and printf(1) does. */
                char expanded[1024];
                unescape(argument, expanded, sizeof(expanded));
                fputs(expanded, stdout);
                continue;
            }
            convert(spec, final, argument);
        }

        /* Round again if there are arguments left and this pass used some;
         * without the second condition a format with no conversions in it
         * would repeat forever. */
        if (at >= argc || at == started_at)
            break;
    }

    fflush(stdout);
    return 0;
}
