/* say - put a message in the corner of the screen.
 *
 * The command form of <notify.h>, which exists so that a shell script can say
 * something without a window: a long build finishing, a backup that went
 * wrong. `sleep 60; say done` is the whole of the use it was written for.
 *
 * The name on the card is who it came from, and defaults to the shell rather
 * than to this program - "From say" would name the messenger.
 */

#include <cli.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    cli_begin(argc, argv, "[-f FROM] MESSAGE...", "f:");
    if (cli_argc() < 1)
        cli_usage();

    /* The words, joined back into a sentence: a message is typed as words and
     * arrives here as arguments, and quoting it to keep it together is a thing
     * nobody remembers to do. */
    char text[256];
    int at = 0;
    for (int i = 0; i < cli_argc(); ++i) {
        const char* word = cli_arg(i);
        const int n = (int)strlen(word);
        if (at + n + 2 >= (int)sizeof(text))
            break;
        if (at > 0)
            text[at++] = ' ';
        memcpy(&text[at], word, (unsigned long)n);
        at += n;
    }
    text[at] = '\0';

    notify(cli_value("-f", "Terminal"), text);
    return 0;
}
