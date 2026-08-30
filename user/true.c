/* true - succeed, saying nothing.
 *
 * `while true; do` and `cmd || true` are what it is for: a command whose only
 * job is its exit status. The shell has it as a builtin too, because a loop
 * that forks once per turn to be told 0 is a loop paying for nothing - this is
 * the one anything that is not the shell finds.
 */
int main(void) { return 0; }
