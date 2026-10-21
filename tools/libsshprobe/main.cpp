#include <libssh/libssh.h>
#include <cstdio>

int main()
{
    const char *version = ssh_version(0);
    if (!version) {
        std::fputs("ssh_version returned null\n", stderr);
        return 1;
    }
    std::printf("%s\n", version);
    return 0;
}
