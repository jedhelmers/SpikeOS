/*
 * gl_test.c — OpenGL demo using TinyGL (software renderer).
 *
 * Renders a spinning colored triangle in a window via TinyGL's
 * software rasterizer, blitting to a WM surface.
 */

#include <kernel/window.h>
#include <kernel/surface.h>
#include <kernel/framebuffer.h>
#include <kernel/timer.h>
#include <kernel/keyboard.h>
#include <stddef.h>
#include <stdio.h>

#include <GL/gl.h>
#include <zbuffer.h>

/* Forward declaration */
void gl_test_run(void);

void gl_test_run(void) {
    /* Create a window — request 324x260 so content area is ~320x240 */
    window_t *win = wm_create_window(100, 60, 324, 260, "OpenGL Demo");
    if (!win || !win->surface) {
        printf("gl_test: failed to create window\n");
        return;
    }

    /* Use the actual content-area surface dimensions */
    int w = (int)win->surface->width;
    int h = (int)win->surface->height;

    printf("gl_test: surface %dx%d\n", w, h);

    /* Initialize TinyGL framebuffer at the exact surface size */
    ZBuffer *zb = ZB_open(w, h, ZB_MODE_RGBA, NULL);
    if (!zb) {
        printf("gl_test: ZB_open failed\n");
        wm_destroy_window(win);
        return;
    }

    /* Initialize TinyGL */
    glInit(zb);

    /* Set up OpenGL state */
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    /* Simple perspective: manual gluPerspective equivalent */
    {
        float aspect = (float)w / (float)h;
        float near_val = 0.1f;
        float far_val = 100.0f;
        float top = near_val * 0.57735f;  /* tan(30 degrees) */
        float bottom = -top;
        float right = top * aspect;
        float left = -right;
        glFrustum(left, right, bottom, top, near_val, far_val);
    }

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);

    /* First compositor pass to show the window */
    wm_redraw_all();

    /* Render loop */
    float angle = 0.0f;
    uint32_t frame = 0;
    uint32_t last_tick = timer_ticks();
    int running = 1;

    while (running) {
        /* Process window manager events (drag, close dot, focus) */
        wm_process_events();

        /* Check if window was closed via close dot */
        if (win->flags & WIN_FLAG_CLOSE_REQ) break;

        /* Check if window was hidden (minimized) */
        if (!(win->flags & WIN_FLAG_VISIBLE)) break;

        /* Poll for 'Q' key to quit */
        key_event_t ev = keyboard_get_event();
        if (ev.type == KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q'))
            break;

        /* Clear */
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* Set up camera */
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -3.0f);
        glRotatef(angle, 0.0f, 1.0f, 0.0f);
        glRotatef(angle * 0.7f, 1.0f, 0.0f, 0.0f);

        /* Draw a colored triangle (front face) */
        glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.0f, 0.0f);   /* Red */
            glVertex3f(0.0f, 1.0f, 0.0f);

            glColor3f(0.0f, 1.0f, 0.0f);   /* Green */
            glVertex3f(-0.866f, -0.5f, 0.0f);

            glColor3f(0.0f, 0.0f, 1.0f);   /* Blue */
            glVertex3f(0.866f, -0.5f, 0.0f);
        glEnd();

        /* Draw a second triangle (back face) */
        glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 1.0f, 0.0f);   /* Yellow */
            glVertex3f(0.0f, -1.0f, 0.1f);

            glColor3f(0.0f, 1.0f, 1.0f);   /* Cyan */
            glVertex3f(0.866f, 0.5f, 0.1f);

            glColor3f(1.0f, 0.0f, 1.0f);   /* Magenta */
            glVertex3f(-0.866f, 0.5f, 0.1f);
        glEnd();

        /* Copy TinyGL framebuffer to the window surface.
         * linesize = bytes per row of the destination buffer.
         */
        ZB_copyFrameBuffer(zb, win->surface->pixels, w * 4);

        /* Composite all windows (proper z-ordering via compositor) */
        wm_redraw_all();

        /* Advance animation */
        angle += 2.0f;
        if (angle >= 360.0f) angle -= 360.0f;
        frame++;

        /* Yield CPU — wait ~33ms (3 ticks at 100Hz) for ~30fps */
        while (timer_ticks() - last_tick < 3)
            __asm__ volatile("hlt");
        last_tick = timer_ticks();
    }

    /* Cleanup */
    glClose();
    ZB_close(zb);

    wm_destroy_window(win);

    /* Refocus shell and redraw */
    window_t *sw = wm_get_shell_window();
    if (sw) {
        wm_focus_window(sw);
        wm_redraw_all();
    }

    printf("gl_test: done (%u frames)\n", frame);
}
