/* screenshot - what is on the screen, as a PNG.
 *
 * The framebuffer holds whatever the window server last composed, so reading
 * it is the whole capture: no cooperation from the server is needed and
 * nothing has to be redrawn. Mapping it is root-only, which is the same rule
 * that stops any process drawing over everyone's windows.
 *
 * Written with img_write_png, which emits stored deflate blocks - so the file
 * is larger than one a compressor would make and is a real PNG that anything
 * can read. A screenful is a few megabytes.
 */

#include <display.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "/root/screen.png";

    struct fb_info fb;
    if (fb_info(&fb) != 0 || fb.width == 0 || fb.height == 0) {
        printf("screenshot: no framebuffer\n");
        return 1;
    }
    if (fb.bits_per_pixel != 32) {
        printf("screenshot: the screen is %u bits per pixel, not 32\n",
               fb.bits_per_pixel);
        return 1;
    }

    const unsigned char* pixels = (const unsigned char*)fb_map();
    if (pixels == 0) {
        /* The likely reason, said plainly rather than as a bare failure. */
        printf("screenshot: cannot map the screen (this needs root)\n");
        return 1;
    }

    uint32_t* shot = (uint32_t*)malloc((unsigned long)fb.width * fb.height * 4);
    if (shot == 0) {
        printf("screenshot: out of memory for %ux%u\n", fb.width, fb.height);
        return 1;
    }

    /* Row by row, because the pitch is not always the width: a scanline can be
     * padded, and copying the whole thing as one block would shear the image
     * progressively down the screen. */
    for (unsigned y = 0; y < fb.height; ++y) {
        const uint32_t* src = (const uint32_t*)(pixels + (unsigned long)y * fb.pitch);
        uint32_t* dst = &shot[(unsigned long)y * fb.width];
        for (unsigned x = 0; x < fb.width; ++x)
            dst[x] = src[x] & 0xFFFFFF;     /* the high byte is not colour */
    }

    if (img_write_png(path, shot, fb.width, fb.height) != 0) {
        printf("screenshot: cannot write %s\n", path);
        return 1;
    }
    printf("%ux%u -> %s\n", fb.width, fb.height, path);
    return 0;
}
