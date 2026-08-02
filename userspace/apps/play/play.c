/* play — CLI audio player / synthesizer. */
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

struct note { uint32_t freq; uint32_t dur; };

static struct note starwars[] = {
    { 440, 500 }, { 440, 500 }, { 440, 500 }, { 349, 350 }, { 523, 150 },
    { 440, 500 }, { 349, 350 }, { 523, 150 }, { 440, 1000 },
    { 0, 0 }
};

static struct note ode[] = {
    { 330, 300 }, { 330, 300 }, { 349, 300 }, { 392, 300 },
    { 392, 300 }, { 349, 300 }, { 330, 300 }, { 294, 300 },
    { 262, 300 }, { 262, 300 }, { 294, 300 }, { 330, 300 },
    { 330, 450 }, { 294, 150 }, { 294, 600 },
    { 0, 0 }
};

static void play_song(const char *name, struct note *song) {
    printf("[play] Playing '%s' via /dev/audio...\n", name);
    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) {
        puts("[play] Error: /dev/audio not available.");
        return;
    }
    for (int i = 0; song[i].freq > 0 || song[i].dur > 0; i++) {
        char cmd[64];
        /* manual itoa for cmd */
        int f = song[i].freq, d = song[i].dur;
        char tmpf[16], tmpd[16];
        int lf = 0, ld = 0;
        if (f == 0) tmpf[lf++] = '0';
        while (f > 0) { tmpf[lf++] = '0' + (f % 10); f /= 10; }
        if (d == 0) tmpd[ld++] = '0';
        while (d > 0) { tmpd[ld++] = '0' + (d % 10); d /= 10; }

        strcpy(cmd, "BEEP ");
        int off = 5;
        while (lf > 0) cmd[off++] = tmpf[--lf];
        cmd[off++] = ' ';
        while (ld > 0) cmd[off++] = tmpd[--ld];
        cmd[off] = '\0';

        write(fd, cmd, strlen(cmd));
        /* busy wait between notes if needed */
        for (volatile int s = 0; s < 1000000; s++) {}
    }
    close(fd);
    puts("[play] Playback complete.");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: play <song>");
        puts("available songs: starwars, ode");
        return 0;
    }
    if (strcmp(argv[1], "starwars") == 0) play_song("Star Wars Theme", starwars);
    else if (strcmp(argv[1], "ode") == 0) play_song("Ode to Joy", ode);
    else printf("play: unknown song '%s'\n", argv[1]);
    return 0;
}
