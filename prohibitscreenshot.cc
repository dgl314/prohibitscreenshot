#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <atomic>
#include <functional>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

#include <X11/Xlib.h>
#include <X11/extensions/composite.h>
#include <X11/X.h>
#include <X11/Xutil.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>


#define RTLD_NEXT ((void *) -1l)
std::atomic<bool> inited {false};

struct TraceInfo
{
    pid_t  main_pid {0};
    std::string main_exe;

    std::unordered_set<std::string>* setWhiteList {nullptr};
    std::unordered_map< xcb_pixmap_t, xcb_drawable_t >* mapPixmap2Window {nullptr};
};

static TraceInfo* traceInfo = NULL;
static const char* etcWhitelistPath = "/etc/screenshot_whitelist.conf";

static xcb_connection_t *dpy = NULL;
static xcb_screen_t *screen = NULL;
static xcb_generic_error_t *err = NULL;

extern "C" {
#define HOOK(fn)  \
    static decltype(::fn)* real_##fn = NULL; \
    do { \
        if (!real_##fn) {  \
            real_##fn = (decltype(::fn)*)dlsym(RTLD_NEXT, #fn); \
        } \
        if (!real_##fn) { \
            fprintf(stderr, "hook %s failed\n", #fn); \
        } \
    } while (0)

#define HOOKV(fn, ver)  \
    static decltype(::fn)* real_##fn = NULL; \
    do { \
        if (!real_##fn) {  \
            real_##fn = (decltype(::fn)*)dlvsym(RTLD_NEXT, #fn, ver); \
        } \
        if (!real_##fn) { \
            fprintf(stderr, "hook %s failed\n", #fn); \
        } \
    } while (0)

//XImage *XGetImage(
//        register Display *dpy,
//        Drawable d,
//        int x,
//        int y,
//        unsigned int width,
//        unsigned int height,
//        unsigned long plane_mask,
//        int format)
//{
//    HOOK(XGetImage);
//    //fprintf(stdout, "------------------ the custom function: XGetImage is called\n");
//    return real_XGetImage(dpy, d, x, y, width, height, plane_mask, format);
//}

inline __attribute__((always_inline))
static void trimString(std::string & str )
{
    int s = str.find_first_not_of(" ");
    int e = str.find_last_not_of(" ");
    str = str.substr(s,e-s+1);
    return;
}

static void setupDisplayAndScreen (
    const char *display_name,
    xcb_connection_t **dpy, /* MODIFIED */
    xcb_screen_t **screen)  /* MODIFIED */
{
    int screen_number, i, err;

    /* Open Display */
    *dpy = xcb_connect (display_name, &screen_number);
    if ((err = xcb_connection_has_error (*dpy)) != 0) {
        fprintf (stderr, "Failed to allocate memory in xcb_connect");
        return;
    }

    if (screen) {
        /* find our screen */
        const xcb_setup_t *setup = xcb_get_setup(*dpy);
        xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(setup);
        int screen_count = xcb_setup_roots_length(setup);
        if (screen_count <= screen_number) {
            //Fatal_Error ("unable to access screen %d, max is %d",
                 //screen_number, screen_count-1 );
        }

        for (i = 0; i < screen_number; i++)
            xcb_screen_next(&screen_iter);
        *screen = screen_iter.data;
    }
}

static xcb_atom_t internAtom(const char *name, bool only_if_exists)
{
    if (!name || *name == 0)
        return XCB_NONE;

    if (!dpy)
        return XCB_NONE;

    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(dpy, only_if_exists, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(dpy, cookie, 0);

    if (!reply)
        return XCB_NONE;

    xcb_atom_t atom = reply->atom;
    free(reply);

    return atom;
}

static std::string atomName(xcb_atom_t atom)
{
    if (!atom)
        return std::string();

    if (!dpy) {
        return std::string();
    }

    xcb_get_atom_name_cookie_t cookie = xcb_get_atom_name_unchecked(dpy, atom);
    xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(dpy, cookie, NULL);
    if (reply) {
        std::string result(xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
        free(reply);
        return result;
    }
    return std::string();
}

static std::unordered_set<std::string> windowProperty(xcb_window_t WId, xcb_atom_t propAtom, xcb_atom_t typeAtom)
{
    if (!dpy)
        return std::unordered_set<std::string>();

    std::unordered_set<std::string> data;
    int offset = 0;
    int remaining = 0;

    do {
        xcb_get_property_cookie_t cookie = xcb_get_property(dpy, false, WId,
                                                            propAtom, typeAtom, offset, 1024);
        xcb_get_property_reply_t *reply = xcb_get_property_reply(dpy, cookie, NULL);
        if (!reply)
            break;

        remaining = 0;

        if (reply->type == typeAtom) {
            int len = xcb_get_property_value_length(reply);
            char *datas = (char *)xcb_get_property_value(reply);
            std::string atomValue(datas, len);
            trimString(atomValue);
            data.insert(atomValue);
            remaining = reply->bytes_after;
            offset += len;

        }

        free(reply);
    } while (remaining > 0);

    return data;
}

static std::vector<std::string> split(std::string src, char a = ',')
{
    std::vector<std::string> vecDst;

    std::string::size_type pos1, pos2;
    pos2 = src.find(a);
    pos1 = 0;
    while (std::string::npos != pos2) {
        vecDst.push_back(src.substr(pos1, pos2 - pos1));
        pos1 = pos2 + 1;
        pos2 = src.find(a, pos1);
    }
    vecDst.push_back(src.substr(pos1));
    return vecDst;
}

static void readEtcWhiteList()
{
    std::ifstream f(etcWhitelistPath);
    if (!f.is_open()) {
        //fprintf(stderr, "Failed to open etc whitelist configure file\n");
        return;
    }

    std::string strLine;
    while(!f.eof()) {
        getline(f, strLine);
        if (strLine.empty()) {
            continue;
        }

        trimString(strLine);
        traceInfo->setWhiteList->emplace(strLine);
    }
    f.close();
}

xcb_void_cookie_t
xcb_create_pixmap (xcb_connection_t *c,
                     uint8_t           depth,
                     xcb_pixmap_t      pid,
                     xcb_drawable_t    drawable,
                     uint16_t          width,
                     uint16_t          height)
{
    HOOK(xcb_create_pixmap);

    if (inited) {
        traceInfo->mapPixmap2Window->emplace(pid, drawable);
    }

    return real_xcb_create_pixmap(c, depth, pid, drawable, width, height);
}

xcb_void_cookie_t
xcb_create_pixmap_checked (xcb_connection_t *c,
                             uint8_t           depth,
                             xcb_pixmap_t      pid,
                             xcb_drawable_t    drawable,
                             uint16_t          width,
                             uint16_t          height)
{
    HOOK(xcb_create_pixmap_checked);

    if (inited) {
        traceInfo->mapPixmap2Window->emplace(pid, drawable);
    }

    return real_xcb_create_pixmap_checked(c, depth, pid, drawable, width, height);
}

#if 0
static bool isProhibitWindowByConfigure(xcb_window_t window) {
    bool bProhibitWindow = false;

    std::unordered_map< xcb_atom_t, std::vector<std::string> >::iterator it = traceInfo->mapEtcConfAtom2Value->begin();
    for (; it != traceInfo->mapEtcConfAtom2Value->end(); it++) {
        xcb_atom_t atomProp = it->first;
        const std::vector<std::string>& confAtomValues = it->second;
        std::unordered_set<std::string> windowAtomValues = windowProperty(window, atomProp, XCB_ATOM_ANY);
        std::unordered_set<std::string>::const_iterator atomValueIt = windowAtomValues.begin();
        for (; atomValueIt != windowAtomValues.end(); atomValueIt++) {
            if (std::find(confAtomValues.begin(), confAtomValues.end(), *atomValueIt) != confAtomValues.end()) {
                bProhibitWindow = true;
                break;
            }
        }
        if (bProhibitWindow) {
            fprintf(stdout, "---------isProhibitWindow is true");
            break;
        }
    }

    return bProhibitWindow;
}
#endif

static bool isBrowser(xcb_window_t window)
{
    bool bBrowser = false;

    xcb_list_properties_cookie_t propertyCookie = xcb_list_properties_unchecked(dpy, window);
    xcb_list_properties_reply_t* propertyReply = xcb_list_properties_reply(dpy, propertyCookie, NULL);
    if (!propertyReply) {
        fprintf(stderr, "Failed to xcb_list_properties_reply");
        return bBrowser;
    }
    int atomLength = xcb_list_properties_atoms_length(propertyReply);
    xcb_atom_t* atoms = xcb_list_properties_atoms(propertyReply);

    for (int i = 0; i < atomLength; i++) {
        xcb_atom_t atom = atoms[i];
        std::string name = atomName(atom);
        if (name == std::string("WM_WINDOW_ROLE")) {
            std::unordered_set<std::string> setAtomValues = windowProperty(window, atom, XCB_ATOM_STRING);
            for (std::unordered_set<std::string>::iterator it = setAtomValues.begin(); it != setAtomValues.end(); it++) {
                if (*it == std::string("browser")) {
                    free(propertyReply);
                    return (bBrowser = true);
                }
            }
        }
    }

    free(propertyReply);

    return bBrowser;
}

static void getAllBrowswerWindowRecursively(std::vector<xcb_window_t>& vecBrowserWindow, xcb_window_t root)
{
    xcb_query_tree_cookie_t cookie = xcb_query_tree_unchecked(dpy, root);
    xcb_query_tree_reply_t* treeReply = xcb_query_tree_reply(dpy, cookie, NULL);
    if (!treeReply) {
        fprintf(stderr, "Failed to xcb_query_tree_reply\n");
        return;
    }
    int childLength = xcb_query_tree_children_length(treeReply);
    if (childLength == 0) {
        if (isBrowser(root)) {
            vecBrowserWindow.push_back(root);
        }
        free(treeReply);
        return;
    }
    xcb_window_t* children = xcb_query_tree_children(treeReply);
    for (int i = 0; i < childLength; i++) {
        xcb_window_t child = children[i];
        if (isBrowser(child)) {
            vecBrowserWindow.push_back(child);
        }
        getAllBrowswerWindowRecursively(vecBrowserWindow, child);
    }

    free(treeReply);
}

static bool needProhibitScreenshot(xcb_drawable_t window)
{
    bool isProhibitScreenshot = false;

    std::vector<xcb_window_t> vecBrowserWindow;
    //fprintf(stdout, "------------------ the custom function: xcb_get_image_unchecked is called, window: 0x%lx\n", window);
    if (window != 0 && window != screen->root && isBrowser(window)) {
        vecBrowserWindow.push_back(window);
    } else if (window == screen->root) {
        getAllBrowswerWindowRecursively(vecBrowserWindow, window);
    }

    for (std::vector<xcb_window_t>::iterator it = vecBrowserWindow.begin(); it != vecBrowserWindow.end(); it++) {
        xcb_window_t browserWindow = *it;
        xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes_unchecked(dpy, browserWindow);
        xcb_get_window_attributes_reply_t* attReply = xcb_get_window_attributes_reply(dpy, cookie, NULL);
        if (!attReply) {
            continue;
        }
        if (attReply->map_state == XCB_MAP_STATE_VIEWABLE) {
            std::unordered_set<std::string> vecAtomValues = windowProperty(browserWindow, internAtom("WM_STATE", true), 0x14c);
            for (std::unordered_set<std::string>::iterator it = vecAtomValues.begin(); it != vecAtomValues.end(); it++) {
                const char* data = (*it).c_str();
                long value = *(const unsigned short*)data;
                if (value == NormalState) {
                    free(attReply);
                    return (isProhibitScreenshot = true);
                }
            }
        }
        free(attReply);
    }

    return isProhibitScreenshot;
}

xcb_get_image_cookie_t
xcb_get_image_unchecked (xcb_connection_t *c,
                           uint8_t           format,
                           xcb_drawable_t    drawable,
                           int16_t           x,
                           int16_t           y,
                           uint16_t          width,
                           uint16_t          height,
                           uint32_t          plane_mask)
{
    HOOK(xcb_get_image_unchecked);
    if (!inited) {
        return real_xcb_get_image_unchecked(c, format, drawable, x, y, width, height, plane_mask);
    }

    if (!dpy || !screen) {
        setupDisplayAndScreen(NULL, &dpy, &screen);
        if (!dpy || !screen)
            return real_xcb_get_image_unchecked(c, format, drawable, x, y, width, height, plane_mask);
    }

//    std::fstream in("/home/dgl/xcb_get_image.log", std::ios::in);
//    std::unordered_set<std::string> setStrings;
//    if (in.is_open()) {
//        std::string strLine;

//        while(!in.eof()) {
//            getline(in, strLine);
//            if (strLine.empty()) {
//                continue;
//            }
//            trimString(strLine);
//            setStrings.insert(strLine);
//        }
//    }

//    in.close();

//    std::fstream out("/home/dgl/xcb_get_image.log", std::ios::app | std::ios::out);
//    if (out.is_open()) {
//        if (setStrings.find(traceInfo->main_exe) == setStrings.end()) {
//            out << traceInfo->main_exe << std::endl;
//        }
//    }

//    out.close();

    xcb_drawable_t window = 0;
    std::unordered_map<xcb_pixmap_t, xcb_drawable_t>::const_iterator find = traceInfo->mapPixmap2Window->find(drawable);
    if (find != traceInfo->mapPixmap2Window->end()) {
        window = find->second;
    }

    if (!needProhibitScreenshot(window)) {
        return real_xcb_get_image_unchecked(c, format, drawable, x, y, width, height, plane_mask);
    }

    xcb_get_image_cookie_t xcb_get;
    xcb_get.sequence = 0;
    return xcb_get;
}

xcb_void_cookie_t
xcb_free_pixmap (xcb_connection_t *c,
                 xcb_pixmap_t      pixmap)
{
    HOOK(xcb_free_pixmap);

    if (inited) {
        std::unordered_map<xcb_pixmap_t, xcb_drawable_t>::iterator find = traceInfo->mapPixmap2Window->find(pixmap);
        if (find != traceInfo->mapPixmap2Window->end()) {
            traceInfo->mapPixmap2Window->erase(find);
        }
    }

    return real_xcb_free_pixmap(c, pixmap);
}

xcb_void_cookie_t
xcb_free_pixmap_checked (xcb_connection_t *c,
                         xcb_pixmap_t      pixmap)
{
    HOOK(xcb_free_pixmap_checked);

    if (inited) {
        std::unordered_map<xcb_pixmap_t, xcb_drawable_t>::iterator find = traceInfo->mapPixmap2Window->find(pixmap);
        if (find != traceInfo->mapPixmap2Window->end()) {
            traceInfo->mapPixmap2Window->erase(find);
        }
    }

    return real_xcb_free_pixmap_checked(c, pixmap);
}

//Pixmap XCompositeNameWindowPixmap (Display *dpy, Window window)
//{
//    Pixmap pixmap;
//    return pixmap;
//}

__attribute__((constructor)) static void init()
{
    traceInfo = new TraceInfo();
    traceInfo->main_pid = getpid();
    {
        char buf[1024];
        ssize_t len;

        char proc_exe[64];
        snprintf(proc_exe, 63, "/proc/%d/exe", traceInfo->main_pid);

        if ((len = readlink(proc_exe, buf, sizeof(buf)-1)) != -1) {
            buf[len] = '\0';
            traceInfo->main_exe = buf;
        }
    }
    //fprintf(stdout, "---------main_pid = %d, exe = [%s], inited: %d\n", traceInfo->main_pid, traceInfo->main_exe.c_str(), inited.operator bool());

    traceInfo->mapPixmap2Window = new std::unordered_map<xcb_pixmap_t, xcb_drawable_t>();
    traceInfo->setWhiteList = new std::unordered_set<std::string>();

    readEtcWhiteList();
    if (!traceInfo->main_exe.empty()) {
        size_t index = traceInfo->main_exe.find_last_of('/');
        std::string progName = traceInfo->main_exe.substr(index+1);
        //fprintf(stdout, "---------%s\n", progName.c_str());
        if (traceInfo->setWhiteList->find(progName) != traceInfo->setWhiteList->end()) {
            return;
        }
    }

    //unsetenv("LD_PRELOAD");
    inited.store(true, std::memory_order_release);
}

__attribute__((destructor)) static void fini()
{
    inited.store(false, std::memory_order_release);

    delete traceInfo;
    traceInfo = NULL;
}

}
