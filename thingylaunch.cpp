/*-
 * Copyright (C) 2003      Matt Johnston <matt@ucc.asn.au>
 * Copyright (C) 2009-2014 Pietro Cerutti <gahr@gahr.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xproto.h>
#include <X11/Xutil.h>
#include <X11/keysymdef.h>

#include <libgen.h>
#include <unistd.h>

#include <cctype>
#include <iostream>
#include <iterator>
#include <sstream>

#include "completion.h"
#include "history.h"
#include "bookmark.h"
#include "util.h"

class Thingylaunch {


    public:
        Thingylaunch();
        ~Thingylaunch();

        void run(int argc, char **argv);

    private:
        void readOptions(int argc, char **argv);
        void createWindow();
        void setupGC();
        void eventLoop();
        void grabHack();
        void redraw();
        bool keypress(xcb_key_press_event_t * keyevent);
        void execcmd();
        void die(std::string msg);

        std::string parseFontDesc();
        uint32_t parseColorName(const std::string& s);
        xcb_query_text_extents_reply_t * getTextExtent(const std::string& s, int len);

    private:

        /* X11 */
        xcb_connection_t  * m_connection;
        xcb_screen_t      * m_screen;
        xcb_window_t        m_win;
        xcb_key_symbols_t * m_keysyms;
        xcb_font_t          m_font;
        xcb_gcontext_t      m_fgGc;
        xcb_gcontext_t      m_bgGc;

        /* User-defined options */
        std::string m_fgColorName;
        std::string m_bgColorName;
        std::string m_fontSize;
        std::vector<std::string> m_fontDesc;

        /* Completion, history, and bookmarks */
        Completion m_comp;
        History    m_hist;
        Bookmark   m_book;

        /* The command */
        std::string m_command;
        std::string::size_type m_cursorPos;

        /* The window size */
        static constexpr uint16_t WindowWidth { 640 };
        static constexpr uint16_t WindowHeight { 25 };
};

Thingylaunch::Thingylaunch()
    : m_fgColorName { "black" },
      m_bgColorName { "white" },
      m_fontSize { "15" },
      m_fontDesc { "*", "*", "medium", "r", "*", "*", "15", "*", "*", "*", "*", "*", "*", "*" },
      m_cursorPos { 0 }
{ }

Thingylaunch::~Thingylaunch()
{
    free(m_keysyms);
    xcb_close_font(m_connection, m_font);
    xcb_free_gc(m_connection, m_fgGc);
    xcb_free_gc(m_connection, m_bgGc);
    xcb_destroy_window(m_connection, m_win);
    xcb_disconnect(m_connection);
}

void
Thingylaunch::run(int argc, char **argv)
{
    readOptions(argc, argv);

    createWindow();

    setupGC();

    grabHack();
    eventLoop();
}

int
main(int argc, char **argv)
{
    Thingylaunch t;
    t.run(argc, argv);

    return 0;
}

void
Thingylaunch::readOptions(int argc, char **argv)
{

#define setParam(varName) { \
    if (i+1 == args.end()) { \
        die("not enough parameters given"); \
    } \
    (varName) = *(i+1); \
    ++i; \
    continue; \
}

    std::vector<std::string> args;
    std::copy(argv+1, argv+argc, std::back_inserter(args));

    for (auto i = args.cbegin(); i != args.cend(); ++i) {
        const auto& s = *i;

        /* background color */
        if (s == "-bg") {
            setParam(m_bgColorName);
        }

        /* foreground color */
        if (s == "-fg") {
            setParam(m_fgColorName);
        }

        /* font foundry */
        if (s == "-fo") {
            setParam(m_fontDesc[0]);
        }

        /* font family */
        if (s == "-ff") {
            setParam(m_fontDesc[1]);
        }

        /* font weight */
        if (s == "-fw") {
            setParam(m_fontDesc[2]);
        }

        /* font slant */
        if (s == "-fs") {
            setParam(m_fontDesc[3]);
        }

        /* font width name */
        if (s == "-fwn") {
            setParam(m_fontDesc[4]);
        }

        /* font style name */
        if (s == "-fsn") {
            setParam(m_fontDesc[5]);
        }

        /* font point size */
        if (s == "-fpt") {
            setParam(m_fontDesc[6]); 
        }
    }
}

void
Thingylaunch::createWindow()
{
    /* open connection to the display server */
    if ((m_connection = xcb_connect(NULL, NULL)) == nullptr) {
        die("Couldn't connect to X11 server");
    }
    m_screen = xcb_setup_roots_iterator(xcb_get_setup(m_connection)).data;

    /* allocate keysyms */
    m_keysyms = xcb_key_symbols_alloc(m_connection);
    if (m_keysyms == nullptr) {
        die("Couldn't allocate key symbols");
    }

    /* figure out the window location */
    int top { m_screen->height_in_pixels / 2 - WindowHeight / 2 };
    int left { m_screen->width_in_pixels / 2 - WindowWidth / 2 };

    /* create the window */
    uint32_t mask { XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK };
    uint32_t value[] { 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS };
    m_win = xcb_generate_id(m_connection);
    auto createCookie = xcb_create_window_checked(m_connection, XCB_COPY_FROM_PARENT, m_win, m_screen->root,
            left, top, WindowWidth, WindowHeight, 10, 0, m_screen->root_visual, mask, value);

    /* set wm hints */
    xcb_size_hints_t hints;
    hints.flags = XCB_ICCCM_SIZE_HINT_P_SIZE | XCB_ICCCM_SIZE_HINT_P_POSITION | XCB_ICCCM_SIZE_HINT_P_SIZE | 
                  XCB_ICCCM_SIZE_HINT_P_MIN_SIZE | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    hints.x = top;
    hints.y = left;
    hints.min_width = hints.max_width = WindowWidth;
    hints.min_height = hints.max_height = WindowHeight;
    xcb_icccm_set_wm_normal_hints_checked(m_connection, m_win, &hints);

    /* map the window */
    auto mapCookie = xcb_map_window_checked(m_connection, m_win);

    if (xcb_request_check(m_connection, createCookie)) {
        die("Couldn't create window");
    }

    if (xcb_request_check(m_connection, mapCookie)) {
        die("Couldn't map window");
    }
}

std::string
Thingylaunch::parseFontDesc()
{
    std::stringstream ss;
    for (auto& i : m_fontDesc)
        ss << "-" << i;
    return ss.str();
}

uint32_t
Thingylaunch::parseColorName(const std::string& colorName)
{
     auto lc = xcb_lookup_color(m_connection, m_screen->default_colormap, colorName.size(), colorName.c_str());
     auto lr = xcb_lookup_color_reply(m_connection, lc, NULL);
     auto ac = xcb_alloc_color(m_connection, m_screen->default_colormap, lr->exact_red, lr->exact_green, lr->exact_blue);
     auto ar = xcb_alloc_color_reply(m_connection, ac, NULL);

     uint32_t color { ar->pixel };

     free(lr);
     free(ar);

     return color;
}

void
Thingylaunch::setupGC()
{
    /* open font */
    auto fontName = parseFontDesc();
    m_font = xcb_generate_id(m_connection);
    auto fontCookie = xcb_open_font_checked(m_connection, m_font, fontName.size(), fontName.c_str());
    if (xcb_request_check(m_connection, fontCookie)) {
        die("Couldn't open font");
    }

    /* resolve colors */
    auto bgColor = parseColorName(m_bgColorName);
    auto fgColor = parseColorName(m_fgColorName);

    /* create gc */
    uint32_t gcMask { XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_LINE_WIDTH | XCB_GC_LINE_STYLE | XCB_GC_CAP_STYLE | XCB_GC_JOIN_STYLE | XCB_GC_FONT };
    uint32_t gcValues[] { fgColor, bgColor, 1, XCB_LINE_STYLE_SOLID, XCB_CAP_STYLE_BUTT, XCB_JOIN_STYLE_BEVEL, m_font };
    m_fgGc = xcb_generate_id(m_connection);
    auto fgGcCookie = xcb_create_gc_checked(m_connection, m_fgGc, m_win, gcMask, gcValues);

    /* create rectangle gc */
    uint32_t rectgcMask { XCB_GC_FOREGROUND | XCB_GC_BACKGROUND };
    uint32_t rectgcValues[] { bgColor, bgColor };
    m_bgGc = xcb_generate_id(m_connection);
    auto bgGcCookie = xcb_create_gc_checked(m_connection, m_bgGc, m_win, rectgcMask, rectgcValues);

    if (xcb_request_check(m_connection, fgGcCookie)) {
        die("Couldn't create GC");
    }
    if (xcb_request_check(m_connection, bgGcCookie)) {
        die("Couldn't create GC");
    }
}

void
Thingylaunch::grabHack()
{
    auto cookie = xcb_grab_keyboard(m_connection, 1, m_win, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    auto reply = xcb_grab_keyboard_reply(m_connection, cookie, NULL);

    if (reply && reply->status == XCB_GRAB_STATUS_SUCCESS) {
        free(reply);
        return;
    }

    xcb_set_input_focus(m_connection, XCB_INPUT_FOCUS_PARENT, m_win, XCB_CURRENT_TIME);
}

void
Thingylaunch::eventLoop()
{
    xcb_generic_event_t * e;
    xcb_key_press_event_t * ev;
    
    redraw();

    while ((e = xcb_wait_for_event(m_connection))) {
        switch (e->response_type & ~0x80) {
            case XCB_EXPOSE:
                redraw();
                break;
            case XCB_KEY_PRESS:
                ev = reinterpret_cast<xcb_key_press_event_t *>(e);
                if (keypress(ev)) {
                    free(e);
                    return;
                }
                break;
            default:
                break;
        }
        free(e);
    }
}

xcb_query_text_extents_reply_t *
Thingylaunch::getTextExtent(const std::string& s, int len)
{
    xcb_char2b_t chars[len];
    for (int i = 0; i < len; ++i) {
        chars[i].byte1 = 0;
        chars[i].byte2 = s[i];
    }

    auto cookie = xcb_query_text_extents(m_connection, m_font, len, chars);
    auto reply = xcb_query_text_extents_reply(m_connection, cookie, NULL);

    return reply;
}

void
Thingylaunch::redraw()
{
    /* draw the background rectangle */
    xcb_rectangle_t extRect { 0, 0, WindowWidth, WindowHeight };
    auto bgCookie = xcb_poly_fill_rectangle_checked(m_connection, m_win, m_bgGc, 1, &extRect);

    /* draw the foreground rectangle */
    xcb_rectangle_t intRect { 0, 0, WindowWidth-1, WindowHeight-1 };
    auto fgCookie = xcb_poly_rectangle_checked(m_connection, m_win, m_fgGc, 1, &intRect);

    /* get text size */
    auto wholeExt = getTextExtent(m_command, m_command.size());
    auto partialExt = getTextExtent(m_command, m_cursorPos);

    /* draw the text */
    auto txtCookie = xcb_image_text_8_checked(m_connection, m_command.size(), m_win, m_fgGc, 2,
        WindowHeight/2 + wholeExt->font_ascent/2, m_command.c_str());

    /* draw the cursor */
    int16_t cursorLeft = partialExt->overall_width + 2;
    xcb_rectangle_t curRect = { cursorLeft, 6, 1, 16 };
    auto curCookie = xcb_poly_fill_rectangle_checked(m_connection, m_win, m_fgGc, 1, &curRect);

    free(wholeExt);
    free(partialExt);

    if (xcb_request_check(m_connection, bgCookie)) {
        die("Couldn't draw");
    }
    if (xcb_request_check(m_connection, fgCookie)) {
        die("Couldn't draw");
    }
    if (xcb_request_check(m_connection, txtCookie)) {
        die("Couldn't draw");
    }
    if (xcb_request_check(m_connection, curCookie)) {
        die("Couldn't draw");
    }

    xcb_flush(m_connection);

}

bool
Thingylaunch::keypress(xcb_key_press_event_t * keyevent)
{
    auto charPressed = xcb_key_symbols_get_keysym(m_keysyms, keyevent->detail, 0);

    /* check for a Shift-key meaning capital letter */
    if (keyevent->state & ShiftMask) {
        charPressed = toupper(charPressed);
    }

    /* check for an Alt-key meaning bookmark lookup */
    if (keyevent->state & Mod1Mask) {
        std::string book = m_book.lookup(charPressed);
        if (!book.empty()) {
            m_command = std::move(book);
            m_hist.save(m_command);
            execcmd();
            return true;
        }
    }

    switch(charPressed) {
        case XK_Escape:
            exit(0);
            break;

        case XK_BackSpace:
            m_comp.reset();
            if (m_cursorPos != 0)
                m_command.erase(--m_cursorPos);
            break;

        case XK_Left:
        case XK_KP_Left:
            if (m_cursorPos != 0)
                --m_cursorPos;
            break;

        case XK_Right:
        case XK_KP_Right:
            if (m_cursorPos < m_command.length())
                ++m_cursorPos;
            break;

        case XK_Up:
        case XK_KP_Up:
            m_command = m_hist.prev();
            m_cursorPos = m_command.length();
            break;

        case XK_Down:
        case XK_KP_Down:
            m_command = m_hist.next();
            m_cursorPos = m_command.length();
            break;

        case XK_Home:
        case XK_KP_Home:
            m_cursorPos = 0;
            break;

        case XK_End:
        case XK_KP_End:
            m_cursorPos = m_command.length();
            break;

        case XK_Return:
            m_hist.save(m_command);
            execcmd();
            return true;
            break;

        case XK_Tab:
        case XK_KP_Tab:
            m_command = m_comp.next(m_command);
            m_cursorPos = m_command.length();
            break;

        case XK_k:
            if (keyevent->state & ControlMask) {
                m_comp.reset();
                m_command.clear();
                m_cursorPos = 0;
                charPressed = 0; // don't handle the 'k' below
            }
            break;

        case XK_w:
            if (keyevent->state & ControlMask) {
                auto i = m_cursorPos - 1;
                while (i > 0) {
                    if (m_command[--i] == ' ') {
                        break;
                    }
                }
                /* do not remove the heading space */
                if (i != 0) {
                    ++i;
                }

                m_command.erase(i, m_cursorPos);

                m_cursorPos = i;
                charPressed = 0; // don't handle the 'w' below
            }
            break;

        default:
            break;
    }

    /* normal printable chars including Latin-[1-8] + Keybad numbers */
    if ((charPressed >= 0x20 && charPressed <= 0x13be) || (charPressed >= 0xffb0 && charPressed <= 0xffb9)) {
        if (m_cursorPos == m_command.length()) {
            m_command.push_back(charPressed);
        } else {
            m_command.insert(m_cursorPos, 1, charPressed);
        }
        ++m_cursorPos;
        m_comp.reset();
    }

    redraw();
    return false;
}

void
Thingylaunch::execcmd()
{
    if (fork()) {
        return;
    }

    std::string shell;
    try {
        shell = Util::getEnv("SHELL");
    } catch (...) {
        shell = "/bin/sh";
    }

    const char * argv[4] { 0 };
    argv[0] = basename(const_cast<char *>(shell.c_str()));
    argv[1] = "-c";
    argv[2] = m_command.c_str();
    argv[3] = NULL;

    execv(shell.c_str(), const_cast<char * const *>(argv));
    /* not reached */
}

void
Thingylaunch::die(std::string msg)
{
    std::cerr << "Error: " << msg << std::endl;
    exit(1);
}

/*
   Copyright (c) 2003 Matt Johnston
   All rights reserved.

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
   */
