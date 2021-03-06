// Problems: Backspaces are sent after the string has been committed
// TODO:
// * Find out where Backspaces are caught
// * Proceed Backspaces before committing

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <libintl.h>

#include <wait.h>
#include <string.h>
#include <X11/Xlib.h>
#include <ibus.h>

#include "engine_const.h"
#include "engine_private.h"
#include "utils.h"
#include "unikey.h"
#include "vnconv.h"
#include "engine.h"

// DEBUG
#include <iostream>
#include <string>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>

using namespace std;

// ibus below version 1.2.99 have problem with PROP_TYPE_NORMAL, use
// RADIO instead
#if !IBUS_CHECK_VERSION(1,2,99)
#define PROP_TYPE_NORMAL PROP_TYPE_RADIO
#endif

#define _(string) gettext(string)

#define CONVERT_BUF_SIZE 1024

static IBusEngineClass* parent_class = NULL;
static IBusConfig*      config       = NULL;

static pthread_t th_mcap;
static pthread_mutex_t mutex_mcap;
static Display* dpy;
static IBusUnikeyEngine* unikey; // current (focus) unikey engine
static gboolean mcap_running;

// cmpitg
static Display* dsp;
static KeyCode IUK_Backspace;

static bool pendingCommitted = false;
static bool pendingClear = false;
static gint nBackspaces = 0;

static string oldPreeditStr = "";

static void ibus_unikey_engine_delete_a_char (IBusEngine *engine) {
    // ibus_engine_delete_surrounding_text (engine, -1, 1);

    // Method #2 -- doesn't work
    // char buf[20];
    // sprintf (buf, "%c %c", 8, 8);
    // ibus_unikey_engine_commit_string (engine, buf);

    // Method #3 -- 0x8 is ASCII Backspace
    // char buf[20];
    // sprintf (buf, "%c", 8);
    // ibus_unikey_engine_commit_string (engine, buf);

    // Method #4 -- 127 is ASCII Delete, got from showkey -a
    // char buf[20];
    // sprintf (buf, "%c", 127);
    // ibus_unikey_engine_commit_string (engine, buf);

    // Method #5
    XTestFakeKeyEvent (dsp, IUK_Backspace, True, 0);
    XTestFakeKeyEvent (dsp, IUK_Backspace, False, 0);
    pendingCommitted = true;
    nBackspaces++;
}

static void ibus_unikey_engine_reset (IBusEngine* engine) {
    // DEBUG
    // cerr << "-- Reset is called --" << endl;
    unikey = (IBusUnikeyEngine*) engine;

    UnikeyResetBuf ();

    // FIXME
    // This is probably not necessary because the string is always committed
    if (unikey->preeditstr->length () > 0) {
        // ibus_unikey_engine_commit_string
        //     (engine, getPreeditStr ().c_str ());
        ibus_unikey_engine_update_preedit_string2
            (engine, getPreeditStr ().c_str () ,true);

        pendingClear = true;
        // unikey->preeditstr->clear ();
    }
    // unikey->preeditstr->clear ();

    parent_class->reset (engine);
}

static void ibus_unikey_engine_commit_string
(IBusEngine *engine, const gchar *string) {
    IBusText *text;

    // BEUG
    cerr << "-- Committing string" << endl;
    text = ibus_text_new_from_static_string (string);
    ibus_engine_commit_text (engine, text);
}

static void ibus_unikey_engine_update_preedit_string2
(IBusEngine *engine, const gchar *string, gboolean visible) {
    // FIXME

    // Delete the old preedit string.  Note that the multibyte-string
    // length is different from usual string length.
    if (getIbusTextLength (oldPreeditStr) > 0) {
        for (guint i = 0; i < getIbusTextLength (oldPreeditStr) + 1; i++) {
            // DEBUG
            cerr << "-- DelChar is called --" << endl;
            ibus_unikey_engine_delete_a_char (engine);
        }
    }

    // Then commit the new one
    if (getIbusTextLength (oldPreeditStr) == 0) {
        ibus_unikey_engine_commit_string (engine, getPreeditStr ().c_str ());
        // oldPreeditStr = getPreeditStr ();
    }

    // DEBUG
    cerr << "[[[ Inside update preedit string ]]]" << endl
         << "* Old string: " << oldPreeditStr << endl
//         << "  Num chars old: " << getIbusTextLength (oldPreeditStr) << endl
         << "* Preedit string: '" << getPreeditStr () << "'" << endl
//         << "  Num chars new: " << getPreeditStr ().length () << endl
         << "---" << endl;
}

// This is called when a char in preedit text is changed
static void ibus_unikey_engine_erase_chars (IBusEngine *engine, int num_chars) {
    int i, k;
    guchar c;
    unikey = (IBusUnikeyEngine*) engine;
    k = num_chars;

    for (i = unikey->preeditstr->length () - 1; i >= 0 && k > 0; i--) {
        c = unikey->preeditstr->at (i);

        // count down if byte is begin byte of utf-8 char
        if (c < (guchar)'\x80' || c >= (guchar)'\xC0') {
            k--;
        }
    }

    unikey->preeditstr->erase (i + 1);
}

static gboolean ibus_unikey_engine_process_key_event
(IBusEngine* engine, guint keyval, guint keycode, guint modifiers) {
    static gboolean tmp;

    unikey = (IBusUnikeyEngine*) engine;

    tmp = ibus_unikey_engine_process_key_event_preedit
        (engine, keyval, keycode, modifiers);

    // check last keyevent with shift
    if (keyval >= IBUS_space && keyval <= IBUS_asciitilde) {
        unikey->last_key_with_shift = modifiers & IBUS_SHIFT_MASK;
    } else {
        unikey->last_key_with_shift = false;
    } // end check last keyevent with shift

    return tmp;
}

static gboolean ibus_unikey_engine_process_key_event_preedit
(IBusEngine* engine, guint keyval, guint keycode, guint modifiers) {
    // DEBUG
    // Don't print the information when it's a RELEASE event
    if (!isKeyRelease(modifiers)) {
        cerr << endl << endl
             << "### Processing keyevent ###" << endl
             << "; Keyval: " << gdk_keyval_name (keyval)
             << "; Keycode: " << keycode
             << "; Modifiers: " << modifierNames (modifiers) << endl
             << "Preedit string before processing: '" << getPreeditStr () << "'" << endl;
    }

    // Don't handle RELEASE key event
    if (modifiers & IBUS_RELEASE_MASK)
        return false;

    // Fake Backspaces processing
    if (isBackspacePressed (keyval) && pendingCommitted) {
        cerr << "    ;;; Fake Backspace caught! " << nBackspaces << " ;;;" << endl;
        if (nBackspaces == 1) {
            ibus_unikey_engine_commit_string (engine, getPreeditStr ().c_str ());
            pendingCommitted = false;
            oldPreeditStr = getPreeditStr ();
            nBackspaces = 0;
            if (pendingClear) {
                unikey->preeditstr->clear();
                oldPreeditStr = "";
                pendingClear = false;
            }
            return true;
        }
        nBackspaces--;
        return false;
    }

    nBackspaces = 0;
    oldPreeditStr = getPreeditStr ();

    if (isBackspacePressed (keyval)) {
        UnikeyBackspacePress ();

        if (nothingToDelete (UnikeyBackspaces, getPreeditStr ())) {
            ibus_unikey_engine_reset (engine);
            return false;
        } else {
            processBackspace (engine);
        }
        return true;
    }

    if (wordIsTerminated (keyval, modifiers)) {
        // ibus_unikey_engine_reset (engine);
        unikey->preeditstr->clear ();
        oldPreeditStr = "";
        UnikeyResetBuf ();
        return false;
    }

    if (isShiftPressed (keyval, modifiers)) {
        return false;
    }

    if (isNumpadKey (keyval)) {
        ibus_unikey_engine_reset (engine);
        return false;
    }

    if (isCharacter (keyval)) {
        UnikeySetCapsState
            (modifiers & IBUS_SHIFT_MASK, modifiers & IBUS_LOCK_MASK);

        //
        // Processing keyval
        //
        // Automatically commit words which never change (such as
        // consonants, ...), but don't commit if macro is enabled.
        if ((!isMacroEnabled ()) && isEndingLetter (keyval)) {
            UnikeyPutChar (keyval);
            return false;
        }

        // FIXME
        // What's this piece of code?
        //
        // if (isUsingTelex ()
        //     && unikey->process_w_at_begin == false
        //     && UnikeyAtWordBeginning ()
        //     && (keyval == IBUS_w || keyval == IBUS_W)) {
        //     // DEBUG
        //     cerr << "^^^ Useless piece of code???" << endl;
        //     UnikeyPutChar (keyval);
        //     if (isMacroEnabled ()) {
        //         return false;
        //     } else {
        //         // DEBUG
        //         unikey->preeditstr->append (keyval == IBUS_w ? "w" : "W");
        //         ibus_unikey_engine_update_preedit_string2
        //             (engine, getPreeditStr ().c_str (), true);
        //         return true;
        //     }
        // }

        // If key event is combination of shift + something
        if (isCombinationOfShift (keyval, modifiers)) {
            UnikeyRestoreKeyStrokes ();
        } else {
            UnikeyFilter (keyval);
        }
        //
        // end of processing keyval
        //

        //
        // Processing result of ukengine
        // This happens when a character needs to be transform (such
        // as adding hook, diacritical marks, ...)
        processUkEngineData (engine, keyval);

        //
        // Commit string when the last character indicates a word is
        // completed
        if (getPreeditStr ().length () > 0) {
            guint lastChar = getPreeditStr ().at (getPreeditStr ().length () - 1);
            // DEBUG
            // cerr << "; Last char: " << lastChar
            //      << "; Is word-break symbol? " << isWordBreakSym (lastChar)
            //      << "; Keyval: " << keyval << endl;
            if (lastChar == keyval && isWordBreakSym (lastChar)) {
                ibus_unikey_engine_reset (engine);
                return true;
            }
        }

        //
        // Commit everything left
        ibus_unikey_engine_update_preedit_string2
            (engine, unikey->preeditstr->c_str (), true);
        return true;
    }

    ibus_unikey_engine_reset (engine);
    return false;
}

void processBackspace (IBusEngine *engine) {
    // Do delete some characters
    if (isOneCharToDelete (getPreeditStr ().length (), UnikeyBackspaces)) {
        ibus_unikey_engine_delete_a_char (engine);
        unikey->preeditstr->clear ();
    } else {
        // DEBUG
        cerr << "; Erase chars and update preedit string" << endl;
        ibus_unikey_engine_erase_chars (engine, UnikeyBackspaces);
        ibus_unikey_engine_update_preedit_string2
            (engine, getPreeditStr ().c_str (), true);
    }

    // Changing the position of the tone after pressing backspace
    if (UnikeyBufChars > 0) {
        adjustTonePosition ();
        ibus_unikey_engine_update_preedit_string2
            (engine, getPreeditStr ().c_str (), true);
    }
}

void adjustTonePosition () {
    if (unikey->oc == CONV_CHARSET_XUTF8) {
        unikey->preeditstr->append
            ((const gchar*) UnikeyBuf, UnikeyBufChars);
    } else {
        static unsigned char buf[CONVERT_BUF_SIZE];
        int bufSize = CONVERT_BUF_SIZE;

        latinToUtf (buf, UnikeyBuf, UnikeyBufChars, &bufSize);
        unikey->preeditstr->append
            ((const gchar*)buf, CONVERT_BUF_SIZE - bufSize);
    }
}

void processUkEngineData (IBusEngine *engine, guint keyval) {
    if (UnikeyBackspaces > 0) {
        if (isOneCharToDelete (getPreeditStr ().length (), UnikeyBackspaces)) {
            unikey->preeditstr->clear ();
        } else {
            ibus_unikey_engine_erase_chars (engine, UnikeyBackspaces);
        }
    }
    if (UnikeyBufChars > 0) {
        if (unikey->oc == CONV_CHARSET_XUTF8) {
            unikey->preeditstr->append
                ((const gchar*) UnikeyBuf, UnikeyBufChars);
        } else {
            static unsigned char buf[CONVERT_BUF_SIZE];
            int bufSize = CONVERT_BUF_SIZE;

            latinToUtf (buf, UnikeyBuf, UnikeyBufChars, &bufSize);
            unikey->preeditstr->append
                ((const gchar*)buf, CONVERT_BUF_SIZE - bufSize);
        }
    } else if (keyval != IBUS_Shift_L && keyval != IBUS_Shift_R) { // if ukengine is not processed
        static int n;
        static char s[6];

        n = g_unichar_to_utf8 (keyval, s); // convert ucs4 to utf8 char
        unikey->preeditstr->append (s, n);
    }
}

static void* thread_mouse_capture (void* data) {
    XEvent event;
    Window w;

    dpy = XOpenDisplay (NULL);
    w = XDefaultRootWindow (dpy);

    while (mcap_running) {
        pthread_mutex_lock (&mutex_mcap);
        XGrabPointer
            (dpy, w, 0, ButtonPressMask | PointerMotionMask,
             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        XPeekEvent (dpy, &event);

        // set mutex to lock status, so process will wait until next unlock
        pthread_mutex_trylock (&mutex_mcap);

        XUngrabPointer (dpy, CurrentTime);
        XSync (dpy, TRUE);
        ibus_unikey_engine_reset ((IBusEngine*)unikey);
    }

    XCloseDisplay (dpy);

    return NULL;
}

static void* thread_run_setup (void* data) {
    int stat;

    popen (LIBEXECDIR "/ibus-setup-unikey --engine", "r");
    wait (&stat);
    if (stat == 0)
        ibus_quit ();
    return NULL;
}

//
// Engine setup
//

GType ibus_unikey_engine_get_type (void) {
    static GType type = 0;

    static const GTypeInfo type_info = {
        sizeof (IBusUnikeyEngineClass),
        (GBaseInitFunc) NULL,
        (GBaseFinalizeFunc) NULL,
        (GClassInitFunc) ibus_unikey_engine_class_init,
        NULL,
        NULL,
        sizeof (IBusUnikeyEngine),
        0,
        (GInstanceInitFunc) ibus_unikey_engine_init,
    };

    if (type == 0) {
        type = g_type_register_static (IBUS_TYPE_ENGINE,
                                       "IBusUnikeyEngine",
                                       &type_info,
                                       (GTypeFlags) 0);
    }

    return type;
}

void ibus_unikey_init (IBusBus* bus) {
    UnikeySetup ();
    config = ibus_bus_get_config (bus);

    mcap_running = TRUE;
    pthread_mutex_init (&mutex_mcap, NULL);
    pthread_mutex_trylock (&mutex_mcap);
    pthread_create (&th_mcap, NULL, &thread_mouse_capture, NULL);
    pthread_detach (th_mcap);
}

void ibus_unikey_exit () {
    mcap_running = FALSE;
    pthread_mutex_unlock (&mutex_mcap); // unlock mutex, so thread can exit
    UnikeyCleanup ();
}

static void ibus_unikey_engine_class_init (IBusUnikeyEngineClass* klass) {
    GObjectClass* object_class         = G_OBJECT_CLASS (klass);
    IBusObjectClass* ibus_object_class = IBUS_OBJECT_CLASS (klass);
    IBusEngineClass* engine_class      = IBUS_ENGINE_CLASS (klass);

    parent_class = (IBusEngineClass* ) g_type_class_peek_parent (klass);

    object_class->constructor = ibus_unikey_engine_constructor;
    ibus_object_class->destroy =
        (IBusObjectDestroyFunc) ibus_unikey_engine_destroy;

    engine_class->process_key_event = ibus_unikey_engine_process_key_event;
    engine_class->reset             = ibus_unikey_engine_reset;
    engine_class->enable            = ibus_unikey_engine_enable;
    engine_class->disable           = ibus_unikey_engine_disable;
    engine_class->focus_in          = ibus_unikey_engine_focus_in;
    engine_class->focus_out         = ibus_unikey_engine_focus_out;
    engine_class->property_activate = ibus_unikey_engine_property_activate;
}

static void ibus_unikey_engine_init (IBusUnikeyEngine* unikey) {
    // FIXME
    gint *argc = 0;
    gchar ***argv = 0;
    gdk_init(argc, argv);

    dsp = (Display *) gdk_x11_get_default_xdisplay ();
    IUK_Backspace = XKeysymToKeycode(dsp, XK_BackSpace);

    gchar* str;
    gboolean b;
    guint i;

    // DEBUG
    unikey->preeditstr = new string ();

//set default options
    unikey->im = Unikey_IM[0];
    unikey->oc = Unikey_OC[0];
    unikey->ukopt.spellCheckEnabled     = DEFAULT_CONF_SPELLCHECK;
    unikey->ukopt.autoNonVnRestore      = DEFAULT_CONF_AUTONONVNRESTORE;
    unikey->ukopt.modernStyle           = DEFAULT_CONF_MODERNSTYLE;
    unikey->ukopt.freeMarking           = DEFAULT_CONF_FREEMARKING;
    unikey->ukopt.macroEnabled          = DEFAULT_CONF_MACROENABLED;
    unikey->process_w_at_begin          = DEFAULT_CONF_PROCESSWATBEGIN;
    unikey->mouse_capture               = DEFAULT_CONF_MOUSECAPTURE;

// read config value
    // read Input Method
    if (ibus_unikey_config_get_string (config, CONFIG_SECTION, CONFIG_INPUTMETHOD, &str)) {
        for (i = 0; i < NUM_INPUTMETHOD; i++) {
            if (strcasecmp (str, Unikey_IMNames[i]) == 0) {
                unikey->im = Unikey_IM[i];
                break;
            }
        }
    } // end read Input Method

    // read Output Charset
    if (ibus_unikey_config_get_string (config, CONFIG_SECTION, CONFIG_OUTPUTCHARSET, &str)) {
        for (i = 0; i < NUM_OUTPUTCHARSET; i++) {
            if (strcasecmp (str, Unikey_OCNames[i]) == 0) {
                unikey->oc = Unikey_OC[i];
                break;
            }
        }
    } // end read Output Charset

    // read Unikey Option
    // freemarking
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_FREEMARKING, &b))
        unikey->ukopt.freeMarking = b;

    // modernstyle
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_MODERNSTYLE, &b))
        unikey->ukopt.modernStyle = b;

    // macroEnabled
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_MACROENABLED, &b))
        unikey->ukopt.macroEnabled = b;

    // spellCheckEnabled
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_SPELLCHECK, &b))
        unikey->ukopt.spellCheckEnabled = b;

    // autoNonVnRestore
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_AUTORESTORENONVN, &b))
        unikey->ukopt.autoNonVnRestore = b;

    // ProcessWAtBegin
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_PROCESSWATBEGIN, &b))
        unikey->process_w_at_begin = b;

    // MouseCapture
    if (ibus_unikey_config_get_boolean (config, CONFIG_SECTION, CONFIG_MOUSECAPTURE, &b))
        unikey->mouse_capture = b;
    // end read Unikey Option
// end read config value

    // load macro
    gchar* fn = get_macro_file ();
    UnikeyLoadMacroTable (fn);
    g_free (fn);

    ibus_unikey_engine_create_property_list (unikey);
}

static GObject* ibus_unikey_engine_constructor (GType type,
                                                guint n_construct_params,
                                                GObjectConstructParam* construct_params) {
    IBusUnikeyEngine* unikey;

    unikey = (IBusUnikeyEngine*)
        G_OBJECT_CLASS (parent_class)->constructor (type,
                                                    n_construct_params,
                                                    construct_params);

    return (GObject*)unikey;
}

static void ibus_unikey_engine_destroy (IBusUnikeyEngine* unikey) {
    // DEBUG
    delete unikey->preeditstr;
    g_object_unref (unikey->prop_list);

    IBUS_OBJECT_CLASS (parent_class)->destroy ((IBusObject*)unikey);
}

static void ibus_unikey_engine_focus_in (IBusEngine* engine) {
    unikey = (IBusUnikeyEngine*)engine;

    UnikeySetInputMethod (unikey->im);
    UnikeySetOutputCharset (unikey->oc);

    UnikeySetOptions (&unikey->ukopt);
    ibus_engine_register_properties (engine, unikey->prop_list);

    parent_class->focus_in (engine);
}

// This is call when a word is committed
static void ibus_unikey_engine_focus_out (IBusEngine* engine) {
    ibus_unikey_engine_reset (engine);
    // DEBUG
    // cerr << "A word is committed!";

    parent_class->focus_out (engine);
}

static void ibus_unikey_engine_enable (IBusEngine* engine) {
    parent_class->enable (engine);
}

static void ibus_unikey_engine_disable (IBusEngine* engine) {
    parent_class->disable (engine);
}

// CUT-HERE

static void ibus_unikey_engine_property_activate (IBusEngine *engine,
                                                  const gchar *prop_name,
                                                  guint prop_state) {
    IBusProperty* prop;
    IBusText* label;
    guint i, j, idxCurrentPropIM;

    unikey = (IBusUnikeyEngine*) engine;

    if (isPropIMActivated (prop_name)) {
        idxCurrentPropIM = getIdxCurrentPropIM (prop_name);
        unikey->im = Unikey_IM[idxCurrentPropIM];
        ibus_unikey_config_set_string (config, CONFIG_SECTION,
                                       CONFIG_INPUTMETHOD,
                                       Unikey_IMNames[idxCurrentPropIM]);
        if (!updatePropIMLabel (unikey, idxCurrentPropIM)) // Failed to update label
            return;
        if (!updatePropIMState (unikey, prop_name)) // Failed to update state
            return;
    }

    else if (isPropOutputCharsetActivated (prop_name)) {
        for (i = 0; i < NUM_OUTPUTCHARSET; i++) {
            if (strcmp (prop_name+strlen (CONFIG_OUTPUTCHARSET) + 1,
                        Unikey_OCNames[i]) == 0) {
                unikey->oc = Unikey_OC[i];

                ibus_unikey_config_set_string (config,
                                               CONFIG_SECTION,
                                               CONFIG_OUTPUTCHARSET,
                                               Unikey_OCNames[i]);

                // update label
                for (j = 0; j < unikey -> prop_list -> properties -> len; j++) {
                    prop = ibus_prop_list_get (unikey->prop_list, j);
                    if (prop==NULL)
                        return;
                    else if (strcmp (prop->key, CONFIG_OUTPUTCHARSET) == 0) {
                        label =
                            ibus_text_new_from_static_string (Unikey_OCNames[i]);
                        ibus_property_set_label (prop, label);
                        break;
                    }
                } // end update label

                // update property state
                for (j = 0; j < unikey->menu_oc->properties->len; j++) {
                    prop = ibus_prop_list_get (unikey->menu_oc, j);
                    if (prop == NULL)
                        return;
                    else if (strcmp (prop->key, prop_name) == 0)
                        prop->state = PROP_STATE_CHECKED;
                    else
                        prop->state = PROP_STATE_UNCHECKED;
                } // end update property state

                break;
            }
        }
    }

    else if (isPropSpellcheckActivated (prop_name)) {
        unikey->ukopt.spellCheckEnabled = !unikey->ukopt.spellCheckEnabled;
        ibus_unikey_config_set_boolean (config,
                                        CONFIG_SECTION,
                                        CONFIG_SPELLCHECK,
                                        unikey->ukopt.spellCheckEnabled);

        // update state
        for (j = 0; j < unikey->menu_opt->properties->len ; j++) {
            prop = ibus_prop_list_get (unikey->menu_opt, j);
            if (prop == NULL)
                return;

            else if (strcmp (prop->key, CONFIG_SPELLCHECK) == 0) {
                prop->state = (unikey->ukopt.spellCheckEnabled == 1)?
                    PROP_STATE_CHECKED:PROP_STATE_UNCHECKED;
                break;
            }
        } // end update state
    }

    else if (isPropMacroEnabledActivated (prop_name)) {
        unikey->ukopt.macroEnabled = !unikey->ukopt.macroEnabled;
        ibus_unikey_config_set_boolean (config,
                                        CONFIG_SECTION,
                                        CONFIG_MACROENABLED,
                                        unikey->ukopt.macroEnabled);

        // update state
        for (j = 0; j < unikey->menu_opt->properties->len ; j++) {
            prop = ibus_prop_list_get (unikey->menu_opt, j);
            if (prop == NULL)
                return;

            else if (strcmp (prop->key, CONFIG_MACROENABLED) == 0) {
                prop->state = (unikey->ukopt.macroEnabled == 1)?
                    PROP_STATE_CHECKED:PROP_STATE_UNCHECKED;
                break;
            }
        } // end update state
    }

    else if (isPropMouseCaptureActivated (prop_name)) {
        unikey->mouse_capture = !unikey->mouse_capture;

        ibus_unikey_config_set_boolean (config,
                                        CONFIG_SECTION,
                                        CONFIG_MOUSECAPTURE,
                                        unikey->mouse_capture);

        // update state
        for (j = 0; j < unikey->menu_opt->properties->len ; j++) {
            prop = ibus_prop_list_get (unikey->menu_opt, j);
            if (prop == NULL)
                return;

            else if (strcmp (prop->key, CONFIG_MOUSECAPTURE) == 0) {
                prop->state = (unikey->mouse_capture == 1)?
                    PROP_STATE_CHECKED:PROP_STATE_UNCHECKED;
                break;
            }
        } // end update state
    }

    else if (isPropRunSetupGUIActivated (prop_name)) {
        pthread_t pid;
        pthread_create (&pid, NULL, &thread_run_setup, NULL);
        pthread_detach (pid);
    }

    ibus_unikey_engine_focus_out (engine);
    ibus_unikey_engine_focus_in (engine);
}

static void ibus_unikey_engine_create_property_list (IBusUnikeyEngine *unikey) {
    unikey->prop_list = ibus_prop_list_new ();
    unikey->menu_im   = ibus_prop_list_new ();
    unikey->menu_oc   = ibus_prop_list_new ();
    unikey->menu_opt  = ibus_prop_list_new ();

    g_object_ref_sink (unikey->prop_list);

    createIMMenu (unikey);
    createOutputCharsetMenu (unikey);
    createOptionMenu (unikey);

    createTopIMMenu (unikey);
    createTopOutputCharsetMenu (unikey);
    createTopOptionMenu (unikey);
}

//
// Helpers
//

static string getPreeditStr () {
    return *(unikey->preeditstr);
}

static bool isMacroEnabled () {
    return (unikey->ukopt.macroEnabled == 0);
}

static bool isUsingTelex () {
    return (unikey->im == UkTelex || unikey->im == UkSimpleTelex2);
}

bool isCombinationOfShift (guint keyval, guint modifiers) {
    return
        (unikey->last_key_with_shift == false && (modifiers & IBUS_SHIFT_MASK)
         && keyval == IBUS_space && !UnikeyAtWordBeginning ())
        || (keyval == IBUS_Shift_L || keyval == IBUS_Shift_R);
}

static guint getIbusTextLength (string str) {
    IBusText *text;
    text = ibus_text_new_from_static_string ((const gchar *) str.c_str ());
    return ibus_text_get_length (text);
}

static void createIMMenu (IBusUnikeyEngine* engine) {
    gchar name[32];

    for (guint i = 0; i < NUM_INPUTMETHOD; i++) {
        sprintf (name, CONFIG_INPUTMETHOD"_%s", Unikey_IMNames[i]);

        addMenu (engine->menu_im, name, PROP_TYPE_RADIO,
                 makeIbusText (Unikey_IMNames[i]), "",
                 makeIbusText (""), TRUE, TRUE,
                 (Unikey_IM[i] == engine->im) ?
                 PROP_STATE_CHECKED : PROP_STATE_UNCHECKED,
                 NULL);
    }
}

static void createOutputCharsetMenu (IBusUnikeyEngine *engine) {
    gchar name[32];

    for (guint i = 0; i < NUM_OUTPUTCHARSET; i++) {
        sprintf (name, CONFIG_OUTPUTCHARSET"_%s", Unikey_OCNames[i]);

        addMenu (engine->menu_oc, name, PROP_TYPE_RADIO,
                 makeIbusText (Unikey_OCNames[i]), "",
                 makeIbusText (""), TRUE, TRUE,
                 (Unikey_OC[i] == engine->oc) ?
                 PROP_STATE_CHECKED : PROP_STATE_UNCHECKED,
                 NULL);
    }

}

static void addMenu (IBusPropList *engineProp,
                     const gchar *key,
                     IBusPropType type,
                     IBusText *label,
                     const gchar *icon,
                     IBusText *tooltip,
                     gboolean sensitive,
                     gboolean visible,
                     IBusPropState state,
                     IBusPropList *prop_list) {
    IBusProperty *prop;

    prop = ibus_property_new (key, type, label, icon, tooltip,
                              sensitive, visible, state, prop_list);
    ibus_prop_list_append (engineProp, prop);
}

static void createOptionMenu (IBusUnikeyEngine *engine) {
    // Spellcheck option
    addMenu (engine->menu_opt, CONFIG_SPELLCHECK, PROP_TYPE_TOGGLE,
             makeIbusText (_("Enable spell check")), "",
             makeIbusText (_("If enable, you can decrease mistake when typing")),
             TRUE, TRUE, (engine->ukopt.spellCheckEnabled == 1) ?
             PROP_STATE_CHECKED : PROP_STATE_UNCHECKED, NULL);

    // Macro option
    addMenu (engine->menu_opt, CONFIG_MACROENABLED, PROP_TYPE_TOGGLE,
             makeIbusText (_("Enable Macro")), "",
             makeIbusText (""),
             TRUE, TRUE, (engine->ukopt.macroEnabled == 1) ?
             PROP_STATE_CHECKED : PROP_STATE_UNCHECKED,
             NULL);


    addMenu (engine->menu_opt, CONFIG_MOUSECAPTURE, PROP_TYPE_TOGGLE,
             makeIbusText (_("Capture mouse event")), "",
             makeIbusText
             (_("Auto send PreEdit string to application when mouse is moved or clicked")),
             TRUE, TRUE, (engine->mouse_capture == 1) ?
             PROP_STATE_CHECKED : PROP_STATE_UNCHECKED,
             NULL);

    // Seperator
    addMenu (engine->menu_opt, "", PROP_TYPE_SEPARATOR,
             NULL, "", NULL,
             TRUE, TRUE, PROP_STATE_UNCHECKED,
             NULL);

    // Launch Setup GUI option
    addMenu (engine->menu_opt, "RunSetupGUI", PROP_TYPE_NORMAL,
             makeIbusText (_("Full setup...")), "",
             makeIbusText (_("Full setup utility for IBus-Unikey")),
             TRUE, TRUE, PROP_STATE_UNCHECKED, NULL);
}

static guint getIdxCurrentIM (IBusUnikeyEngine *engine) {
    for (guint i = 0; i < NUM_INPUTMETHOD; i++)
        if (Unikey_IM[i] == engine->im)
            return i;
    return 0;
}

static void createTopIMMenu (IBusUnikeyEngine *engine) {
    addMenu (engine->prop_list, CONFIG_INPUTMETHOD, PROP_TYPE_MENU,
             makeIbusText (Unikey_IMNames[getIdxCurrentIM (engine)]), "",
             makeIbusText (_("Choose input method")),
             TRUE, TRUE, PROP_STATE_UNCHECKED, engine->menu_im);
}

static guint getIdxCurrentOutputCharset (IBusUnikeyEngine *engine) {
    for (guint i = 0; i < NUM_OUTPUTCHARSET; i++)
        if (Unikey_OC[i] == engine->oc)
            return i;
    return 0;
}

static void createTopOutputCharsetMenu (IBusUnikeyEngine *engine) {
    addMenu (engine->prop_list, CONFIG_OUTPUTCHARSET, PROP_TYPE_MENU,
             makeIbusText
             (Unikey_OCNames[getIdxCurrentOutputCharset (engine)]), "",
             makeIbusText (_("Choose output charset")),
             TRUE, TRUE, PROP_STATE_UNCHECKED, engine->menu_oc);
}

static void createTopOptionMenu (IBusUnikeyEngine *engine) {
    addMenu (engine->prop_list, "Options", PROP_TYPE_MENU,
             makeIbusText (_("Options")), "gtk-preferences",
             makeIbusText (_("Options for Unikey")),
             TRUE, TRUE, PROP_STATE_UNCHECKED, engine->menu_opt);
}

IBusText *makeIbusText (const gchar *str) {
    return ibus_text_new_from_static_string (str);
}

static bool isPropIMActivated (const gchar *prop_name) {
    return strncmp (prop_name, CONFIG_INPUTMETHOD,
                    strlen (CONFIG_INPUTMETHOD)) == 0;
}

static bool isPropOutputCharsetActivated (const gchar *prop_name) {
    return strncmp (prop_name, CONFIG_OUTPUTCHARSET,
                    strlen (CONFIG_OUTPUTCHARSET)) == 0;
}

static bool isPropSpellcheckActivated (const gchar *prop_name) {
    return strcmp (prop_name, CONFIG_SPELLCHECK) == 0;
}

static bool isPropMacroEnabledActivated (const gchar *prop_name) {
    return strcmp (prop_name, CONFIG_MACROENABLED) == 0;
}

static bool isPropMouseCaptureActivated (const gchar *prop_name) {
    return strcmp (prop_name, CONFIG_MOUSECAPTURE) == 0;
}

static bool isPropRunSetupGUIActivated (const gchar *prop_name) {
    return strcmp (prop_name, "RunSetupGUI") == 0;
}

static guint getIdxCurrentPropIM (const gchar *prop_name) {
    for (guint i = 0; i < NUM_INPUTMETHOD; i++)
        if (strcmp (prop_name + strlen (CONFIG_INPUTMETHOD) + 1,
                    Unikey_IMNames[i]) == 0)
            return i;
    return 0;
}

static bool updatePropIMLabel (IBusUnikeyEngine *unikey, guint idxIM) {
    IBusProperty *prop;

    for (guint i = 0; i < unikey->prop_list->properties->len; i++) {
        prop = ibus_prop_list_get (unikey->prop_list, i);
        if (prop == NULL)
            return false;
        else if (strcmp (prop->key, CONFIG_INPUTMETHOD) == 0) {
            ibus_property_set_label (prop, makeIbusText
                                     (Unikey_IMNames[idxIM]));
            break;
        }
    }
    return true;
}

static bool updatePropIMState (IBusUnikeyEngine *unikey,
                               const gchar *prop_name) {
    IBusProperty *prop;

    for (guint i = 0; i < unikey->menu_im->properties->len; i++) {
        prop = ibus_prop_list_get (unikey->menu_im, i);
        if (prop == NULL)
            return false;
        else if (strcmp (prop->key, prop_name) == 0)
            prop->state = PROP_STATE_CHECKED;
        else
            prop->state = PROP_STATE_UNCHECKED;
    }
    return true;
}
