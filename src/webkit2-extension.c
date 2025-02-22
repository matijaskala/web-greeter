/*
 * webkit2-extension.c
 *
 * Copyright © 2014-2016 Antergos Developers <dev@antergos.com>
 *
 * Includes Code Contributed By:
 * Copyright © 2016 Scott Balneaves <sbalneav@ltsp.org>
 *
 * Based on code from lightdm-webkit-greeter:
 * Copyright © 2010-2014 Robert Ancell <robert.ancell@canonical.com>
 *
 * This file is part of lightdm-webkit2-greeter.
 *
 * lightdm-webkit2-greeter is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * lightdm-webkit2-greeter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * The following additional terms are in effect as per Section 7 of the license:
 *
 * The preservation of all legal notices and author attributions in
 * the material or in the Appropriate Legal Notices displayed
 * by works containing it is required.
 *
 * You should have received a copy of the GNU General Public License
 * along with lightdm-webkit2-greeter; If not, see <http://www.gnu.org/licenses/>.
 */

/* WebKitWebExtension
 * This extension runs inside the web process allowing us to interact directly with
 * the web page including the DOM and JavaScriptWorld.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "config.h"

#ifdef HAS_WEBKITGTK_2_16
#include <webkitdom/webkitdom.h>
#else
#include <webkitdom/WebKitDOMCustom.h>
#endif

#include <webkit2/webkit-web-extension.h>
#include <JavaScriptCore/JavaScript.h>
#include <lightdm.h>

/* Work-around CLion bug */
#ifndef CONFIG_DIR
#include "../build/src/config.h"
#endif


/* Convenience macros for use in functions that can be called from JavaScript
 * running in the web process. They are used to confirm that calls were made by one of
 * the classes that we injected into the web page.
 */
#define USER     ((LightDMUser *)     JSObjectGetPrivate (thisObject))
#define LAYOUT   ((LightDMLayout *)   JSObjectGetPrivate (thisObject))
#define SESSION  ((LightDMSession *)  JSObjectGetPrivate (thisObject))
#define GREETER  ((LightDMGreeter *)  JSObjectGetPrivate (thisObject))
#define LANGUAGE ((LightDMLanguage *) JSObjectGetPrivate (thisObject))


/*
 * Error messages
 */
#define EXPECTSTRING   "Expected a string"
#define ARGNOTSUPPLIED "Argument(s) not supplied"

G_MODULE_EXPORT void webkit_web_extension_initialize(WebKitWebExtension *extension);


guint64 page_id;
GKeyFile *keyfile;

static GSList* paths = NULL, *iter = NULL;

static JSClassRef
	lightdm_greeter_class,
	gettext_class,
	lightdm_user_class,
	lightdm_language_class,
	lightdm_layout_class,
	lightdm_session_class,
	greeter_config_class,
	theme_utils_class;

static gboolean
	detect_theme_errors,
	secure_mode,
	SESSION_STARTING;

static gchar
	*background_images_dir,
	*user_image,
	*logo;

static WebKitWebExtension *WEB_EXTENSION;


/*
 * Returns either a string or null.
 *
 * When passed a const gchar string, either return a JSValueRef string or,
 * if the string is NULL, a JSValueRef Null reference. Used to ensure functions
 * return proper string args.
 */
static JSValueRef
string_or_null(JSContextRef context, const gchar *str) {
	JSValueRef result;
	JSStringRef string;

	if (str == NULL) {
		return JSValueMakeNull(context);
	}

	string = JSStringCreateWithUTF8CString(str);
	result = JSValueMakeString(context, string);
	JSStringRelease(string);

	return result;
}


static void
_mkexception(JSContextRef context, JSValueRef *exception, const gchar *str) {
	JSValueRef result;
	JSStringRef string = JSStringCreateWithUTF8CString(str);
	JSValueRef exceptionString = JSValueMakeString(context, string);
	JSStringRelease(string);
	result = JSValueToObject(context, exceptionString, exception);

	if (result != NULL) {
		*exception = result;
	}
}


/*
 * Makes an Exception.
 *
 * Convert a const string to an exception which can be passed back to webkit.
 */
static JSValueRef
mkexception(JSContextRef context, JSValueRef *exception, const gchar *str) {
	_mkexception(context, exception, str);

	return JSValueMakeNull(context);
}


/*
 * Converts an argument to a string.
 *
 * Convert a JSValueRef argument to a g_malloc'd gchar string. Calling function
 * is responsible for g_freeing the string.
 */
static gchar *
arg_to_string(JSContextRef context, JSValueRef arg, JSValueRef *exception) {
	JSStringRef string;
	size_t size;
	gchar *result;

	if (JSValueGetType(context, arg) != kJSTypeString) {
		_mkexception(context, exception, EXPECTSTRING);

		return NULL;
	}

	string = JSValueToStringCopy(context, arg, exception);

	if (!string) {

		return NULL;
	}

	size = JSStringGetMaximumUTF8CStringSize(string);
	result = g_malloc(size);

	if (!result) {

		return NULL;
	}

	JSStringGetUTF8CString(string, result, size);
	JSStringRelease(string);

	return result;
}


/*
 * g_strreplace
 *
 * Replace one substring with another.  NOTE:  This has the side effect
 * of freeing it's passed text.
 */
static gchar *
g_strreplace (gchar *txt, gchar *from, gchar *to) {
	gchar **split;
	gchar *result;

	split = g_strsplit (txt, from, -1);
	g_free (txt);
	result = g_strjoinv (to, split);
	g_strfreev (split);
	return result;
}


/*
 * Escapes single quote characters in a string.
 *
 * Simple escape function to make sure strings have any/all single
 * quote characters escaped.
 */
static char *
escape(const gchar *text) {
	gchar *escaped;
	gchar *result;

	/* Make sure all newlines, tabs, etc. are escaped. */
	escaped = g_strescape (text, NULL);

	/* Replace ' with \\' */
	result = g_strreplace (escaped, "'", "\\'");

	return result;
}


static JSValueRef
get_user_name_cb(JSContextRef context,
				 JSObjectRef thisObject,
				 JSStringRef propertyName,
				 JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_name(USER));
}


static JSValueRef
get_user_real_name_cb(JSContextRef context,
					  JSObjectRef thisObject,
					  JSStringRef propertyName,
					  JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_real_name(USER));
}


static JSValueRef
get_user_display_name_cb(JSContextRef context,
						 JSObjectRef thisObject,
						 JSStringRef propertyName,
						 JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_display_name(USER));
}


static JSValueRef
get_user_home_directory_cb(JSContextRef context,
						   JSObjectRef thisObject,
						   JSStringRef propertyName,
						   JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_home_directory(USER));
}


static JSValueRef
get_user_image_cb(JSContextRef context,
				  JSObjectRef thisObject,
				  JSStringRef propertyName,
				  JSValueRef *exception) {

	const gchar *image = lightdm_user_get_image(USER);
	gchar *image_path;
	gint  result;

	// Determine if we already checked this path
	for (iter = paths; iter; iter = iter->next) {
		if (0 == g_strcmp0(image, iter->data)) {
			// We've already checked this path, no need to continue further.
			return string_or_null(context, image);
		}
	}

	image_path = g_strdup(image);
	result = g_access(image_path, R_OK);

	if (0 == result) {
		// Path is accessible. Add it to our paths list.
		paths = g_slist_prepend(paths, image_path);

		return string_or_null(context, image);
	}

	// Path is not accessible.
	g_free(image_path);

	return JSValueMakeNull(context);
}


static JSValueRef
get_user_language_cb(JSContextRef context,
					 JSObjectRef thisObject,
					 JSStringRef propertyName,
					 JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_language(USER));
}


static JSValueRef
get_user_layout_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_layout(USER));
}


static JSValueRef
get_user_session_cb(JSContextRef context,
					JSObjectRef thisObject,
					JSStringRef propertyName,
					JSValueRef *exception) {
	return string_or_null(context, lightdm_user_get_session(USER));
}


static JSValueRef
get_user_logged_in_cb(JSContextRef context,
					  JSObjectRef thisObject,
					  JSStringRef propertyName,
					  JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_user_get_logged_in(USER));
}


static JSValueRef
get_language_code_cb(JSContextRef context,
					 JSObjectRef thisObject,
					 JSStringRef propertyName,
					 JSValueRef *exception) {
	return string_or_null(context, lightdm_language_get_code(LANGUAGE));
}


static JSValueRef
get_language_name_cb(JSContextRef context,
					 JSObjectRef thisObject,
					 JSStringRef propertyName,
					 JSValueRef *exception) {
	return string_or_null(context, lightdm_language_get_name(LANGUAGE));
}


static JSValueRef
get_language_territory_cb(JSContextRef context,
						  JSObjectRef thisObject,
						  JSStringRef propertyName,
						  JSValueRef *exception) {
	return string_or_null(context, lightdm_language_get_territory(LANGUAGE));
}


static JSValueRef
get_layout_name_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {
	return string_or_null(context, lightdm_layout_get_name(LAYOUT));
}


static JSValueRef
get_layout_short_description_cb(JSContextRef context,
								JSObjectRef thisObject,
								JSStringRef propertyName,
								JSValueRef *exception) {
	return string_or_null(context, lightdm_layout_get_short_description(LAYOUT));
}


static JSValueRef
get_layout_description_cb(JSContextRef context,
						  JSObjectRef thisObject,
						  JSStringRef propertyName,
						  JSValueRef *exception) {
	return string_or_null(context, lightdm_layout_get_description(LAYOUT));
}


static JSValueRef
get_session_key_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {
	return string_or_null(context, lightdm_session_get_key(SESSION));
}


static JSValueRef
get_session_name_cb(JSContextRef context,
					JSObjectRef thisObject,
					JSStringRef propertyName,
					JSValueRef *exception) {
	return string_or_null(context, lightdm_session_get_name(SESSION));
}


static JSValueRef
get_session_comment_cb(JSContextRef context,
					   JSObjectRef thisObject,
					   JSStringRef propertyName,
					   JSValueRef *exception) {
	return string_or_null(context, lightdm_session_get_comment(SESSION));
}


static JSValueRef
get_hostname_cb(JSContextRef context,
				JSObjectRef thisObject,
				JSStringRef propertyName,
				JSValueRef *exception) {
	return string_or_null(context, lightdm_get_hostname());
}


static JSValueRef
get_num_users_cb(JSContextRef context,
				 JSObjectRef thisObject,
				 JSStringRef propertyName,
				 JSValueRef *exception) {
	return JSValueMakeNumber(
		context,
		g_list_length(lightdm_user_list_get_users(lightdm_user_list_get_instance()))
	);
}


static JSValueRef
get_users_cb(JSContextRef context,
			 JSObjectRef thisObject,
			 JSStringRef propertyName,
			 JSValueRef *exception) {

	JSObjectRef array;
	const GList *users, *link;
	guint i, n_users = 0;
	JSValueRef *args;

	users = lightdm_user_list_get_users(lightdm_user_list_get_instance());
	n_users = g_list_length((GList *) users);
	args = g_malloc(sizeof(JSValueRef) * ( n_users + 1 ));

	for (i = 0, link = users; link; i++, link = link->next) {
		LightDMUser *user = link->data;
		g_object_ref(user);
		args[i] = JSObjectMake(context, lightdm_user_class, user);
	}

	array = JSObjectMakeArray(context, n_users, args, exception);
	g_free(args);

	if (array == NULL) {
		return JSValueMakeNull(context);
	} else {
		return array;
	}
}


static JSValueRef
get_languages_cb(JSContextRef context,
				 JSObjectRef thisObject,
				 JSStringRef propertyName,
				 JSValueRef *exception) {

	JSObjectRef array;
	const GList *languages, *link;
	guint i, n_languages = 0;
	JSValueRef *args;

	languages = lightdm_get_languages();
	n_languages = g_list_length((GList *) languages);
	args = g_malloc(sizeof(JSValueRef) * ( n_languages + 1 ));

	for (i = 0, link = languages; link; i++, link = link->next) {
		LightDMLanguage *language = link->data;
		g_object_ref(language);
		args[i] = JSObjectMake(context, lightdm_language_class, language);
	}

	array = JSObjectMakeArray(context, n_languages, args, exception);
	g_free(args);

	if (array == NULL) {
		return JSValueMakeNull(context);
	} else {
		return array;
	}
}


static JSValueRef
get_language_cb(JSContextRef context,
				JSObjectRef thisObject,
				JSStringRef propertyName,
				JSValueRef *exception) {
	return string_or_null(context, lightdm_language_get_name(lightdm_get_language()));
}


static JSValueRef
get_layouts_cb(JSContextRef context,
			   JSObjectRef thisObject,
			   JSStringRef propertyName,
			   JSValueRef *exception) {

	JSObjectRef array;
	const GList *layouts, *link;
	guint i, n_layouts = 0;
	JSValueRef *args;

	layouts = lightdm_get_layouts();
	n_layouts = g_list_length((GList *) layouts);
	args = g_malloc(sizeof(JSValueRef) * ( n_layouts + 1 ));

	for (i = 0, link = layouts; link; i++, link = link->next) {
		LightDMLayout *layout = link->data;
		g_object_ref(layout);
		args[i] = JSObjectMake(context, lightdm_layout_class, layout);
	}

	array = JSObjectMakeArray(context, n_layouts, args, exception);
	g_free(args);

	if (array == NULL) {
		return JSValueMakeNull(context);
	} else {
		return array;
	}
}


static JSValueRef
get_layout_cb(JSContextRef context,
			  JSObjectRef thisObject,
			  JSStringRef propertyName,
			  JSValueRef *exception) {
	return string_or_null(context, lightdm_layout_get_name(lightdm_get_layout()));
}


static bool
set_layout_cb(JSContextRef context,
			  JSObjectRef thisObject,
			  JSStringRef propertyName,
			  JSValueRef value,
			  JSValueRef *exception) {

	gchar *layout;
	const GList *layouts, *link;
	layout = arg_to_string(context, value, exception);

	if (!layout) {
		return false;
	}

	layouts = lightdm_get_layouts();

	for (link = layouts; link; link = link->next) {
		LightDMLayout *currlayout = link->data;

		if (!( g_strcmp0(lightdm_layout_get_name(currlayout), layout))) {
			g_object_ref(currlayout);
			lightdm_set_layout(currlayout);
			break;
		}
	}

	g_free(layout);

	return true;
}


static JSValueRef
get_sessions_cb(JSContextRef context,
				JSObjectRef thisObject,
				JSStringRef propertyName,
				JSValueRef *exception) {

	JSObjectRef array;
	const GList *sessions, *link;
	guint i, n_sessions = 0;
	JSValueRef *args;

	sessions = lightdm_get_sessions();
	n_sessions = g_list_length((GList *) sessions);
	args = g_malloc(sizeof(JSValueRef) * ( n_sessions + 1 ));

	for (i = 0, link = sessions; link; i++, link = link->next) {
		LightDMSession *session = link->data;
		g_object_ref(session);

		args[i] = JSObjectMake(context, lightdm_session_class, session);
	}

	array = JSObjectMakeArray(context, n_sessions, args, exception);
	g_free(args);

	if (array == NULL) {
		return JSValueMakeNull(context);
	} else {
		return array;
	}
}


static JSValueRef
get_session_starting_cb(JSContextRef context,
						JSObjectRef thisObject,
						JSStringRef propertyName,
						JSValueRef *exception) {

	return JSValueMakeBoolean(context, SESSION_STARTING);
}


static JSValueRef
get_default_session_cb(JSContextRef context,
					   JSObjectRef thisObject,
					   JSStringRef propertyName,
					   JSValueRef *exception) {
	return string_or_null(context, lightdm_greeter_get_default_session_hint(GREETER));
}


static JSValueRef
get_lock_hint_cb(JSContextRef context,
				 JSObjectRef thisObject,
				 JSStringRef propertyName,
				 JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_lock_hint(GREETER));
}


static JSValueRef
get_autologin_timeout_cb(JSContextRef context,
						 JSObjectRef thisObject,
						 JSStringRef propertyName,
						 JSValueRef *exception) {
	return JSValueMakeNumber(context, lightdm_greeter_get_autologin_timeout_hint(GREETER));
}


static JSValueRef
cancel_autologin_cb(JSContextRef context,
					JSObjectRef function,
					JSObjectRef thisObject,
					size_t argumentCount,
					const JSValueRef arguments[],
					JSValueRef *exception) {

	lightdm_greeter_cancel_autologin(GREETER);

	return JSValueMakeNull(context);
}


static JSValueRef
authenticate_cb(JSContextRef context,
				JSObjectRef function,
				JSObjectRef thisObject,
				size_t argumentCount,
				const JSValueRef arguments[],
				JSValueRef *exception) {

	gchar *name = NULL;

	if (argumentCount > 0) {
		name = arg_to_string(context, arguments[0], exception);
	}

	#ifdef HAS_LIGHTDM_1_19_2
	GError *err = NULL;

	lightdm_greeter_authenticate(GREETER, name, &err);

	if (NULL != err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}
	#else
	lightdm_greeter_authenticate(GREETER, name);
	#endif

	g_free(name);

	return JSValueMakeNull(context);
}


static JSValueRef
authenticate_as_guest_cb(JSContextRef context,
						 JSObjectRef function,
						 JSObjectRef thisObject,
						 size_t argumentCount,
						 const JSValueRef arguments[],
						 JSValueRef *exception) {

	#ifdef HAS_LIGHTDM_1_19_2
	GError *err = NULL;

	lightdm_greeter_authenticate_as_guest(GREETER, &err);

	if (NULL != err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}
	#else
	lightdm_greeter_authenticate_as_guest(GREETER);
	#endif

	return JSValueMakeNull(context);
}


static JSValueRef
get_hint_cb(JSContextRef context,
			JSObjectRef function,
			JSObjectRef thisObject,
			size_t argumentCount,
			const JSValueRef arguments[],
			JSValueRef *exception) {

	gchar *hint_name = NULL;
	JSValueRef result;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	hint_name = arg_to_string(context, arguments[0], exception);

	if (!hint_name) {
		return JSValueMakeNull(context);
	}

	result = string_or_null(context, lightdm_greeter_get_hint(GREETER, hint_name));
	g_free(hint_name);

	return result;
}


static JSValueRef
respond_cb(JSContextRef context,
		   JSObjectRef function,
		   JSObjectRef thisObject,
		   size_t argumentCount,
		   const JSValueRef arguments[],
		   JSValueRef *exception) {

	gchar *response = NULL;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	response = arg_to_string(context, arguments[0], exception);

	if (!response) {
		return JSValueMakeNull(context);
	}

	#ifdef HAS_LIGHTDM_1_19_2
	GError *err = NULL;

	lightdm_greeter_respond(GREETER, response, &err);

	if (NULL != err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}
	#else
	lightdm_greeter_respond(GREETER, response);
	#endif

	g_free(response);

	return JSValueMakeNull(context);
}


static JSValueRef
cancel_authentication_cb(JSContextRef context,
						 JSObjectRef function,
						 JSObjectRef thisObject,
						 size_t argumentCount,
						 const JSValueRef arguments[],
						 JSValueRef *exception) {

	#ifdef HAS_LIGHTDM_1_19_2
	GError *err = NULL;

	lightdm_greeter_cancel_authentication(GREETER, &err);

	if (NULL != err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}
	#else
	lightdm_greeter_cancel_authentication(GREETER);
	#endif

	return JSValueMakeNull(context);
}


static JSValueRef
get_authentication_user_cb(JSContextRef context,
						   JSObjectRef thisObject,
						   JSStringRef propertyName,
						   JSValueRef *exception) {
	return string_or_null(context, lightdm_greeter_get_authentication_user(GREETER));
}


static JSValueRef
get_has_guest_account_cb(JSContextRef context,
						 JSObjectRef thisObject,
						 JSStringRef propertyName,
						 JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_has_guest_account_hint(GREETER));
}


static JSValueRef
get_hide_users_cb(JSContextRef context,
				  JSObjectRef thisObject,
				  JSStringRef propertyName,
				  JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_hide_users_hint(GREETER));
}


static JSValueRef
get_select_user_hint_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {
	return string_or_null(context, lightdm_greeter_get_select_user_hint(GREETER));
}


static JSValueRef
get_select_guest_hint_cb(JSContextRef context,
					JSObjectRef thisObject,
					JSStringRef propertyName,
					JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_select_guest_hint(GREETER));
}


static JSValueRef
get_autologin_user_cb(JSContextRef context,
					  JSObjectRef thisObject,
					  JSStringRef propertyName,
					  JSValueRef *exception) {
	return string_or_null(context, lightdm_greeter_get_autologin_user_hint(GREETER));
}


static JSValueRef
get_autologin_guest_cb(JSContextRef context,
					   JSObjectRef thisObject,
					   JSStringRef propertyName,
					   JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_autologin_guest_hint(GREETER));
}


static JSValueRef
get_is_authenticated_cb(JSContextRef context,
						JSObjectRef thisObject,
						JSStringRef propertyName,
						JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_is_authenticated(GREETER));
}


static JSValueRef
get_in_authentication_cb(JSContextRef context,
						 JSObjectRef thisObject,
						 JSStringRef propertyName,
						 JSValueRef *exception) {
	return JSValueMakeBoolean(context, lightdm_greeter_get_in_authentication(GREETER));
}


static JSValueRef
get_can_suspend_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {

	return JSValueMakeBoolean(context, lightdm_get_can_suspend());
}


static JSValueRef
suspend_cb(JSContextRef context,
		   JSObjectRef function,
		   JSObjectRef thisObject,
		   size_t argumentCount,
		   const JSValueRef arguments[],
		   JSValueRef *exception) {

	lightdm_suspend(NULL);

	return JSValueMakeNull(context);
}


static JSValueRef
get_can_hibernate_cb(JSContextRef context,
					 JSObjectRef thisObject,
					 JSStringRef propertyName,
					 JSValueRef *exception) {

	return JSValueMakeBoolean(context, lightdm_get_can_hibernate());
}


static JSValueRef
hibernate_cb(JSContextRef context,
			 JSObjectRef function,
			 JSObjectRef thisObject,
			 size_t argumentCount,
			 const JSValueRef arguments[],
			 JSValueRef *exception) {

	lightdm_hibernate(NULL);

	return JSValueMakeNull(context);
}


static JSValueRef
get_can_restart_cb(JSContextRef context,
				   JSObjectRef thisObject,
				   JSStringRef propertyName,
				   JSValueRef *exception) {

	return JSValueMakeBoolean(context, lightdm_get_can_restart());
}


static JSValueRef
restart_cb(JSContextRef context,
		   JSObjectRef function,
		   JSObjectRef thisObject,
		   size_t argumentCount,
		   const JSValueRef arguments[],
		   JSValueRef *exception) {

	lightdm_restart(NULL);

	return JSValueMakeNull(context);
}


static JSValueRef
get_can_shutdown_cb(JSContextRef context,
					JSObjectRef thisObject,
					JSStringRef propertyName,
					JSValueRef *exception) {

	return JSValueMakeBoolean(context, lightdm_get_can_shutdown());
}


static JSValueRef
shutdown_cb(JSContextRef context,
			JSObjectRef function,
			JSObjectRef thisObject,
			size_t argumentCount,
			const JSValueRef arguments[],
			JSValueRef *exception) {

	lightdm_shutdown(NULL);

	return JSValueMakeNull(context);
}


static JSValueRef
start_session_cb(JSContextRef context,
				 JSObjectRef function,
				 JSObjectRef thisObject,
				 size_t argumentCount,
				 const JSValueRef arguments[],
				 JSValueRef *exception) {

	gchar *session = NULL;
	gboolean result;
	GError *err = NULL;

	WebKitDOMDOMWindow *dom_window;
	WebKitDOMDocument *dom_document;
	WebKitWebPage *web_page;

	/* FIXME: old API required lightdm.login(username, session), but the username
	 * is never actually used.  At some point, deprecate the old usage.  For now,
	 * simply work around it.
	 */
	if (argumentCount == 1) {
		session = arg_to_string(context, arguments[0], exception);
	} else if (argumentCount == 2) {
		session = arg_to_string(context, arguments[1], exception);
	}

	web_page = webkit_web_extension_get_page(WEB_EXTENSION, page_id);

	if (NULL != web_page) {
		dom_document = webkit_web_page_get_dom_document(web_page);
		dom_window = webkit_dom_document_get_default_view(dom_document);

		if (dom_window) {
			/* Stop theme heartbeat */
			webkit_dom_dom_window_webkit_message_handlers_post_message(
				dom_window, "GreeterBridge", "Heartbeat::Exit"
			);
		}
	}

	SESSION_STARTING = TRUE;

	result = lightdm_greeter_start_session_sync(GREETER, session, &err);
	g_free(session);

	if (err != NULL) {
		SESSION_STARTING = FALSE;
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}

	return JSValueMakeBoolean(context, result);
}


static JSValueRef
set_language_cb(JSContextRef context,
				JSObjectRef function,
				JSObjectRef thisObject,
				size_t argumentCount,
				const JSValueRef arguments[],
				JSValueRef *exception) {

	gchar *language = NULL;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	language = arg_to_string(context, arguments[0], exception);

	if (!language) {
		return JSValueMakeNull(context);
	}

	#ifdef HAS_LIGHTDM_1_19_2
	GError *err = NULL;

	lightdm_greeter_set_language(GREETER, language, &err);

	if (NULL != err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
	}
	#else
	lightdm_greeter_set_language(GREETER, language);
	#endif

	g_free(language);

	return JSValueMakeNull(context);
}


static JSValueRef
gettext_cb(JSContextRef context,
		   JSObjectRef function,
		   JSObjectRef thisObject,
		   size_t argumentCount,
		   const JSValueRef arguments[],
		   JSValueRef *exception) {

	gchar *string = NULL;
	JSValueRef result;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	string = arg_to_string(context, arguments[0], exception);

	if (!string) {
		return JSValueMakeNull(context);
	}

	result = string_or_null(context, gettext(string));
	g_free(string);

	return result;
}


static JSValueRef
ngettext_cb(JSContextRef context,
			JSObjectRef function,
			JSObjectRef thisObject,
			size_t argumentCount,
			const JSValueRef arguments[],
			JSValueRef *exception) {

	gchar *string = NULL, *plural_string = NULL;
	unsigned int n = 0;
	JSValueRef result;

	if (argumentCount != 3) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	string = arg_to_string(context, arguments[0], exception);

	if (!string) {
		return JSValueMakeNull(context);
	}

	plural_string = arg_to_string(context, arguments[1], exception);

	if (!plural_string) {
		return JSValueMakeNull(context);
	}

	n = JSValueToNumber(context, arguments[2], exception);
	result = string_or_null(context, ngettext(string, plural_string, n));

	g_free(string);
	g_free(plural_string);

	return result;
}

/*
 * Gets a key's value from config file.
 *
 * Returns value as a string.
 */
static JSValueRef
get_conf_str_cb(JSContextRef context,
				JSObjectRef function,
				JSObjectRef thisObject,
				size_t argumentCount,
				const JSValueRef arguments[],
				JSValueRef *exception) {

	gchar *section, *key, *value;
	GError *err = NULL;
	JSValueRef result;

	if (argumentCount != 2) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	section = arg_to_string(context, arguments[0], exception);
	if (!section) {
		return JSValueMakeNull(context);
	}

	key = arg_to_string(context, arguments[1], exception);
	if (!key) {
		return JSValueMakeNull(context);
	}

	if (0 == g_strcmp0(key, "themes_dir")) {
		value = g_strdup_printf("%s", THEME_DIR);

	} else if (0 == g_strcmp0(key, "lightdm_data_dir")) {
		#ifdef HAS_LIGHTDM_1_19_2
		value = lightdm_greeter_ensure_shared_data_dir_sync(GREETER, section, &err);
		#else
		value = lightdm_greeter_ensure_shared_data_dir_sync(GREETER, section);
		#endif

	} else {
		value = g_key_file_get_string(keyfile, section, key, &err);
	}

	if (err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
		return JSValueMakeNull(context);
	}

	result = string_or_null(context, value);

	g_free(value);

	return result;
}


/*
 * Gets a key's value from config file.
 *
 * Returns value as a number.
 */
static JSValueRef
get_conf_num_cb(JSContextRef context,
				JSObjectRef function,
				JSObjectRef thisObject,
				size_t argumentCount,
				const JSValueRef arguments[],
				JSValueRef *exception) {

	gchar *section, *key;
	gint value;
	GError *err = NULL;

	if (argumentCount != 2) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	section = arg_to_string(context, arguments[0], exception);
	if (!section) {
		return JSValueMakeNull(context);
	}

	key = arg_to_string(context, arguments[1], exception);
	if (!key) {
		return JSValueMakeNull(context);
	}

	value = g_key_file_get_integer(keyfile, section, key, &err);

	if (err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
		return JSValueMakeNull(context);
	}

	return JSValueMakeNumber(context, value);
}


/*
 * Gets a key's value from config file.
 *
 * Returns value as a bool.
 */
static JSValueRef
get_conf_bool_cb(JSContextRef context,
				 JSObjectRef function,
				 JSObjectRef thisObject,
				 size_t argumentCount,
				 const JSValueRef arguments[],
				 JSValueRef *exception) {

	gchar *section, *key;
	gboolean value;
	GError *err = NULL;

	if (argumentCount != 2) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	section = arg_to_string(context, arguments[0], exception);
	if (!section) {
		return JSValueMakeNull(context);
	}

	key = arg_to_string(context, arguments[1], exception);
	if (!key) {
		return JSValueMakeNull(context);
	}

	value = g_key_file_get_boolean(keyfile, section, key, &err);

	if (err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
		return JSValueMakeNull(context);
	}

	return JSValueMakeBoolean(context, value);
}


static JSValueRef
get_dirlist_cb(JSContextRef context,
			   JSObjectRef function,
			   JSObjectRef thisObject,
			   size_t argumentCount,
			   const JSValueRef arguments[],
			   JSValueRef *exception) {

	JSObjectRef array;
	guint n_entries = 0;
	JSValueRef *args = NULL;
	GDir *dir;
	gchar *path, *fullpath;
	const gchar *dirent;
	GError *err = NULL;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	path = arg_to_string(context, arguments[0], exception);

	if (!path) {
		return JSValueMakeNull(context);
	}

	dir = g_dir_open(path, 0, &err);

	if (err) {
		_mkexception(context, exception, err->message);
		g_error_free(err);
		return JSValueMakeNull(context);
	}

	/*
	 * Create the list of the directory entries
	 */
	while ( ( dirent = g_dir_read_name(dir) ) != NULL) {
		n_entries++;
		args = g_realloc( args, sizeof(JSValueRef) * ( n_entries + 1 ) );
		fullpath = g_build_filename(path, dirent, NULL); /* Give theme developer full pathname */
		args[( n_entries - 1 )] = string_or_null(context, fullpath);
		g_free(fullpath);
	}

	g_dir_close(dir);

	array = JSObjectMakeArray(context, n_entries, args, exception);

	g_free(args);

	if (array == NULL) {
		return JSValueMakeNull(context);
	} else {
		return array;
	}
}


static JSValueRef
txt2html_cb(JSContextRef context,
			JSObjectRef function,
			JSObjectRef thisObject,
			size_t argumentCount,
			const JSValueRef arguments[],
			JSValueRef *exception) {
	gchar *txt;
	JSValueRef result;

	if (argumentCount != 1) {
		return mkexception(context, exception, ARGNOTSUPPLIED);
	}

	txt = arg_to_string(context, arguments[0], exception);
	if (!txt) {
		return JSValueMakeNull(context);
	}

	/* Replace & with &amp; */
	txt = g_strreplace (txt, "&", "&amp;");

	/* Replace " with &quot; */
	txt = g_strreplace (txt, "\"", "&quot;");

	/* Replace < with &lt; */
	txt = g_strreplace (txt, "<", "&lt;");

	/* Replace > with &gt; */
	txt = g_strreplace (txt, ">", "&gt;");

	/* Replace newlines with <br> */
	txt = g_strreplace (txt, "\n", "<br>");

	result = string_or_null (context, txt);
	g_free (txt);

	return result;
}


static gchar *
remove_query_and_hash(gchar *str) {
	gchar *ptr = NULL;

	ptr = strchr(str, '?');

	if (NULL != ptr) {
		*ptr = '\0';
	}

	ptr = strchr(str, '#');

	if (NULL != ptr) {
		*ptr = '\0';
	}

	return g_strstrip(str);
}


static const JSStaticValue lightdm_user_values[] = {
	{"display_name",   get_user_display_name_cb,   NULL, kJSPropertyAttributeReadOnly},
	{"home_directory", get_user_home_directory_cb, NULL, kJSPropertyAttributeReadOnly},
	{"image",          get_user_image_cb,          NULL, kJSPropertyAttributeReadOnly},
	{"language",       get_user_language_cb,       NULL, kJSPropertyAttributeReadOnly},
	{"layout",         get_user_layout_cb,         NULL, kJSPropertyAttributeReadOnly},
	{"logged_in",      get_user_logged_in_cb,      NULL, kJSPropertyAttributeReadOnly},
	{"session",        get_user_session_cb,        NULL, kJSPropertyAttributeReadOnly},
	{"username",       get_user_name_cb,           NULL, kJSPropertyAttributeReadOnly},
	/* ---->>> DEPRECATED! <<<------>>> DEPRECATED! <<<------->>> DEPRECATED! <<<----*/
	{"name",           get_user_name_cb,           NULL, kJSPropertyAttributeReadOnly},
	{"real_name",      get_user_real_name_cb,      NULL, kJSPropertyAttributeReadOnly},
	/* ---->>> DEPRECATED! <<<------>>> DEPRECATED! <<<------->>> DEPRECATED! <<<----*/
	{NULL,             NULL,                       NULL, 0}};

static const JSStaticValue lightdm_language_values[] = {
	{"code",      get_language_code_cb,      NULL, kJSPropertyAttributeReadOnly},
	{"name",      get_language_name_cb,      NULL, kJSPropertyAttributeReadOnly},
	{"territory", get_language_territory_cb, NULL, kJSPropertyAttributeReadOnly},
	{NULL,        NULL,                      NULL, 0}};

static const JSStaticValue lightdm_layout_values[] = {
	{"name",              get_layout_name_cb,              NULL, kJSPropertyAttributeReadOnly},
	{"short_description", get_layout_short_description_cb, NULL, kJSPropertyAttributeReadOnly},
	{"description",       get_layout_description_cb,       NULL, kJSPropertyAttributeReadOnly},
	{NULL,                NULL,                            NULL, 0}};

static const JSStaticValue lightdm_session_values[] = {
	{"key",     get_session_key_cb,     NULL, kJSPropertyAttributeReadOnly},
	{"name",    get_session_name_cb,    NULL, kJSPropertyAttributeReadOnly},
	{"comment", get_session_comment_cb, NULL, kJSPropertyAttributeReadOnly},
	{NULL,      NULL,                   NULL, 0}};

static const JSStaticValue lightdm_greeter_values[] = {
	{"authentication_user", get_authentication_user_cb, NULL,            kJSPropertyAttributeReadOnly},
	{"autologin_guest",     get_autologin_guest_cb,     NULL,            kJSPropertyAttributeReadOnly},
	{"autologin_timeout",   get_autologin_timeout_cb,   NULL,            kJSPropertyAttributeReadOnly},
	{"autologin_user",      get_autologin_user_cb,      NULL,            kJSPropertyAttributeReadOnly},
	{"can_hibernate",       get_can_hibernate_cb,       NULL,            kJSPropertyAttributeReadOnly},
	{"can_restart",         get_can_restart_cb,         NULL,            kJSPropertyAttributeReadOnly},
	{"can_shutdown",        get_can_shutdown_cb,        NULL,            kJSPropertyAttributeReadOnly},
	{"can_suspend",         get_can_suspend_cb,         NULL,            kJSPropertyAttributeReadOnly},
	{"default_session",     get_default_session_cb,     NULL,            kJSPropertyAttributeReadOnly},
	{"has_guest_account",   get_has_guest_account_cb,   NULL,            kJSPropertyAttributeReadOnly},
	{"hide_users",          get_hide_users_cb,          NULL,            kJSPropertyAttributeReadOnly},
	{"hostname",            get_hostname_cb,            NULL,            kJSPropertyAttributeReadOnly},
	{"in_authentication",   get_in_authentication_cb,   NULL,            kJSPropertyAttributeReadOnly},
	{"is_authenticated",    get_is_authenticated_cb,    NULL,            kJSPropertyAttributeReadOnly},
	{"language",            get_language_cb,            NULL,            kJSPropertyAttributeReadOnly},
	{"languages",           get_languages_cb,           NULL,            kJSPropertyAttributeReadOnly},
	{"layout",              get_layout_cb,              set_layout_cb,   kJSPropertyAttributeNone},
	{"layouts",             get_layouts_cb,             NULL,            kJSPropertyAttributeReadOnly},
	{"lock_hint",           get_lock_hint_cb,           NULL,            kJSPropertyAttributeReadOnly},
	{"num_users",           get_num_users_cb,           NULL,            kJSPropertyAttributeReadOnly},
	{"select_guest_hint",   get_select_guest_hint_cb,   NULL,            kJSPropertyAttributeReadOnly},
	{"select_user_hint",    get_select_user_hint_cb,    NULL,            kJSPropertyAttributeReadOnly},
	{"sessions",            get_sessions_cb,            NULL,            kJSPropertyAttributeReadOnly},
	{"session_starting",    get_session_starting_cb,    NULL,            kJSPropertyAttributeReadOnly},
	{"users",               get_users_cb,               NULL,            kJSPropertyAttributeReadOnly},
	/* ------>>> DEPRECATED! <<<----------->>> DEPRECATED! <<<------------>>> DEPRECATED! <<<------*/
	{"default_language",    get_language_cb,            NULL,            kJSPropertyAttributeReadOnly},
	{"default_layout",      get_layout_cb,              NULL,            kJSPropertyAttributeReadOnly},
    {"select_guest",        get_select_guest_hint_cb,   NULL,            kJSPropertyAttributeReadOnly},
    {"select_user",         get_select_user_hint_cb,    NULL,            kJSPropertyAttributeReadOnly},
	{"timed_login_delay",   get_autologin_timeout_cb,   NULL,            kJSPropertyAttributeReadOnly},
	{"timed_login_user",    get_autologin_user_cb,      NULL,            kJSPropertyAttributeReadOnly},
	/* ------>>> DEPRECATED! <<<----------->>> DEPRECATED! <<<------------>>> DEPRECATED! <<<------*/
	{NULL,                  NULL,                       NULL,            0}};

static const JSStaticFunction lightdm_greeter_functions[] = {
	{"authenticate",          authenticate_cb,          kJSPropertyAttributeReadOnly},
	{"authenticate_as_guest", authenticate_as_guest_cb, kJSPropertyAttributeReadOnly},
	{"cancel_authentication", cancel_authentication_cb, kJSPropertyAttributeReadOnly},
	{"cancel_autologin",      cancel_autologin_cb,      kJSPropertyAttributeReadOnly},
	{"get_hint",              get_hint_cb,              kJSPropertyAttributeReadOnly},
	{"hibernate",             hibernate_cb,             kJSPropertyAttributeReadOnly},
	{"respond",               respond_cb,               kJSPropertyAttributeReadOnly},
	{"restart",               restart_cb,               kJSPropertyAttributeReadOnly},
	{"set_language",          set_language_cb,          kJSPropertyAttributeReadOnly},
	{"shutdown",              shutdown_cb,              kJSPropertyAttributeReadOnly},
	{"start_session",         start_session_cb,         kJSPropertyAttributeReadOnly},
	{"suspend",               suspend_cb,               kJSPropertyAttributeReadOnly},
	/* -------->>> DEPRECATED! <<<---------------------->>> DEPRECATED! <<<---------*/
	{"cancel_timed_login",    cancel_autologin_cb,      kJSPropertyAttributeReadOnly},
	{"login",                 start_session_cb,         kJSPropertyAttributeReadOnly},
	{"provide_secret",        respond_cb,               kJSPropertyAttributeReadOnly},
	{"start_session_sync",    start_session_cb,         kJSPropertyAttributeReadOnly},
	/* -------->>> DEPRECATED! <<<---------------------->>> DEPRECATED! <<<---------*/
	{NULL,                    NULL,                     0}};

static const JSStaticFunction gettext_functions[] = {
	{"gettext",  gettext_cb,  kJSPropertyAttributeReadOnly},
	{"ngettext", ngettext_cb, kJSPropertyAttributeReadOnly},
	{NULL,       NULL,        0}};


static const JSStaticFunction greeter_config_functions[] = {
	{"get_str",  get_conf_str_cb,  kJSPropertyAttributeReadOnly},
	{"get_num",  get_conf_num_cb,  kJSPropertyAttributeReadOnly},
	{"get_bool", get_conf_bool_cb, kJSPropertyAttributeReadOnly},
	{NULL,       NULL,             0}};


static const JSStaticFunction theme_utils_functions[] = {
	{"dirlist",  get_dirlist_cb,   kJSPropertyAttributeReadOnly},
	{"txt2html", txt2html_cb,      kJSPropertyAttributeReadOnly},
	{NULL,       NULL,             0}};


static const JSClassDefinition lightdm_user_definition = {
	0,                     /* Version       */
	kJSClassAttributeNone, /* Attributes    */
	"LightDMUser",         /* Class name    */
	NULL,                  /* Parent class  */
	lightdm_user_values,   /* Static values */
};

static const JSClassDefinition lightdm_language_definition = {
	0,                       /* Version       */
	kJSClassAttributeNone,   /* Attributes    */
	"LightDMLanguage",       /* Class name    */
	NULL,                    /* Parent class  */
	lightdm_language_values, /* Static values */
};

static const JSClassDefinition lightdm_layout_definition = {
	0,                     /* Version       */
	kJSClassAttributeNone, /* Attributes    */
	"LightDMLayout",       /* Class name    */
	NULL,                  /* Parent class  */
	lightdm_layout_values, /* Static values */
};

static const JSClassDefinition lightdm_session_definition = {
	0,                      /* Version       */
	kJSClassAttributeNone,  /* Attributes    */
	"LightDMSession",       /* Class name    */
	NULL,                   /* Parent class  */
	lightdm_session_values, /* Static values */
};

static const JSClassDefinition lightdm_greeter_definition = {
	0,                         /* Version          */
	kJSClassAttributeNone,     /* Attributes       */
	"__LightDMGreeter",          /* Class name       */
	NULL,                      /* Parent class     */
	lightdm_greeter_values,    /* Static values    */
	lightdm_greeter_functions, /* Static functions */
};

static const JSClassDefinition gettext_definition = {
	0,                     /* Version          */
	kJSClassAttributeNone, /* Attributes       */
	"GettextClass",        /* Class name       */
	NULL,                  /* Parent class     */
	NULL,                  /* Static values    */
	gettext_functions,     /* Static functions */
};

static const JSClassDefinition greeter_config_definition = {
	0,                        /* Version          */
	kJSClassAttributeNone,    /* Attributes       */
	"__GreeterConfig",        /* Class name       */
	NULL,                     /* Parent class     */
	NULL,                     /* Static values    */
	greeter_config_functions, /* Static functions */
};

static const JSClassDefinition theme_utils_definition = {
	0,                     /* Version          */
	kJSClassAttributeNone, /* Attributes       */
	"__ThemeUtils",        /* Class name       */
	NULL,                  /* Parent class     */
	NULL,                  /* Static values    */
	theme_utils_functions, /* Static functions */
};


static void
window_object_cleared_callback(WebKitScriptWorld *world,
							   WebKitWebPage *web_page,
							   WebKitFrame *frame,
							   LightDMGreeter *greeter) {

	JSGlobalContextRef jsContext;
	WebKitDOMDOMWindow *dom_window;
	WebKitDOMDocument *dom_document;

	JSObjectRef gettext_object,
				lightdm_greeter_object,
				greeter_config_object,
				theme_utils_object,
				globalObject;

	jsContext = webkit_frame_get_javascript_context_for_script_world(frame, world);
	globalObject = JSContextGetGlobalObject(jsContext);

	gettext_class = JSClassCreate(&gettext_definition);
	lightdm_greeter_class = JSClassCreate(&lightdm_greeter_definition);
	lightdm_user_class = JSClassCreate(&lightdm_user_definition);
	lightdm_language_class = JSClassCreate(&lightdm_language_definition);
	lightdm_layout_class = JSClassCreate(&lightdm_layout_definition);
	lightdm_session_class = JSClassCreate(&lightdm_session_definition);
	greeter_config_class = JSClassCreate(&greeter_config_definition);
	theme_utils_class = JSClassCreate(&theme_utils_definition);

	gettext_object = JSObjectMake(jsContext, gettext_class, NULL);
	JSObjectSetProperty(jsContext,
						globalObject,
						JSStringCreateWithUTF8CString("gettext"),
						gettext_object,
						kJSPropertyAttributeNone,
						NULL);

	lightdm_greeter_object = JSObjectMake(jsContext, lightdm_greeter_class, greeter);
	JSObjectSetProperty(jsContext,
						globalObject,
						JSStringCreateWithUTF8CString("__LightDMGreeter"),
						lightdm_greeter_object,
						kJSPropertyAttributeNone,
						NULL);

	greeter_config_object = JSObjectMake(jsContext, greeter_config_class, greeter);
	JSObjectSetProperty(jsContext,
						globalObject,
						JSStringCreateWithUTF8CString("__GreeterConfig"),
						greeter_config_object,
						kJSPropertyAttributeDontEnum | kJSPropertyAttributeReadOnly,
						NULL);

	theme_utils_object = JSObjectMake(jsContext, theme_utils_class, NULL);
	JSObjectSetProperty(jsContext,
						globalObject,
						JSStringCreateWithUTF8CString("__ThemeUtils"),
						theme_utils_object,
						kJSPropertyAttributeDontEnum | kJSPropertyAttributeReadOnly,
						NULL);

	dom_document = webkit_web_page_get_dom_document(web_page);
	dom_window = webkit_dom_document_get_default_view(dom_document);

	if (dom_window) {
		/* If the greeter was started as a lock-screen, notify our UI process. */
		if (lightdm_greeter_get_lock_hint(greeter)) {
			webkit_dom_dom_window_webkit_message_handlers_post_message(
				dom_window, "GreeterBridge", "LockHint"
			);
		}
	}
}


static void
show_prompt_cb(LightDMGreeter *greeter,
			   const gchar *text,
			   LightDMPromptType type,
			   WebKitWebExtension *extension) {

	WebKitWebPage *web_page;
	WebKitFrame *web_frame;
	JSGlobalContextRef jsContext;
	JSStringRef command;
	gchar *string;
	gchar *etext;
	const gchar *ct = "";

	web_page = webkit_web_extension_get_page(extension, page_id);

	if (web_page != NULL) {
		web_frame = webkit_web_page_get_main_frame(web_page);
		jsContext = webkit_frame_get_javascript_global_context(web_frame);

		switch (type) {
			case LIGHTDM_PROMPT_TYPE_QUESTION:
				ct = "text";
				break;
			case LIGHTDM_PROMPT_TYPE_SECRET:
				ct = "password";
				break;
		}

		etext = escape(text);
		string = g_strdup_printf("show_prompt('%s', '%s')", etext, ct);
		command = JSStringCreateWithUTF8CString(string);

		JSEvaluateScript(jsContext, command, NULL, NULL, 0, NULL);

		g_free(string);
		g_free(etext);
	}
}


static void
show_message_cb(LightDMGreeter *greeter,
				const gchar *text,
				LightDMMessageType type,
				WebKitWebExtension *extension) {

	WebKitWebPage *web_page;
	WebKitFrame *web_frame;
	JSGlobalContextRef jsContext;
	JSStringRef command;
	gchar *etext;
	gchar *string;
	const gchar *mt = "";

	web_page = webkit_web_extension_get_page(extension, page_id);

	if (web_page != NULL) {
		web_frame = webkit_web_page_get_main_frame(web_page);
		jsContext = webkit_frame_get_javascript_global_context(web_frame);

		switch (type) {
			case LIGHTDM_MESSAGE_TYPE_ERROR:
				mt = "error";
				break;
			case LIGHTDM_MESSAGE_TYPE_INFO:
				mt = "info";
				break;
		}

		etext = escape(text);
		string = g_strdup_printf("show_prompt('%s', '%s')", etext, mt);
		command = JSStringCreateWithUTF8CString(string);

		JSEvaluateScript(jsContext, command, NULL, NULL, 0, NULL);

		g_free(string);
		g_free(etext);
	}
}


static void
authentication_complete_cb(LightDMGreeter *greeter, WebKitWebExtension *extension) {

	WebKitWebPage *web_page;
	WebKitFrame *web_frame;
	JSGlobalContextRef jsContext;
	JSStringRef command;

	web_page = webkit_web_extension_get_page(extension, page_id);

	if (web_page != NULL) {
		web_frame = webkit_web_page_get_main_frame(web_page);
		jsContext = webkit_frame_get_javascript_global_context(web_frame);
		command = JSStringCreateWithUTF8CString("authentication_complete()");

		JSEvaluateScript(jsContext, command, NULL, NULL, 0, NULL);
	}
}


static void
autologin_timer_expired_cb(LightDMGreeter *greeter, WebKitWebExtension *extension) {

	WebKitWebPage *web_page;
	WebKitFrame *web_frame;
	JSGlobalContextRef jsContext;
	JSStringRef command;

	web_page = webkit_web_extension_get_page(extension, page_id);

	if (web_page != NULL) {
		web_frame = webkit_web_page_get_main_frame(web_page);
		jsContext = webkit_frame_get_javascript_global_context(web_frame);
		command = JSStringCreateWithUTF8CString("autologin_timer_expired()");

		JSEvaluateScript(jsContext, command, NULL, NULL, 0, NULL);
	}
}


static gboolean
get_config_option_as_bool(const gchar *section, const gchar *key, GError **err) {
	GError *err_tmp = NULL;
	gboolean result = g_key_file_get_boolean(keyfile, section, key, &err_tmp);

	if (NULL != err_tmp) {
		g_propagate_error(err, err_tmp);
	}

	return result;
}


static gchar*
get_config_option_as_string(const gchar *section, const gchar *key) {
	gchar *value;
	GError *err = NULL;

	value = g_key_file_get_string(keyfile, section, key, &err);

	if (NULL != err) {
		g_error("%s", err->message);
		g_error_free(err);
		g_free(value);
		return "";
	}

	return value;
}


static gboolean
should_block_request(const char *file_path) {
	gboolean result = TRUE; /* Blocked */
	char *canonical_path;

	if (NULL == file_path) {
		return result;
	}

	canonical_path = canonicalize_file_name(file_path);

	if (NULL != canonical_path) {
		for (iter = paths; iter; iter = iter->next) {
			if (0 == g_strcmp0(canonical_path, iter->data) || g_str_has_prefix(canonical_path, iter->data)) {
				result = FALSE; /* Allowed */
				break;
			}
		}
	}

	g_free(canonical_path);

	return result;
}


/**
 * Callback for the "send-request" signal of a {@link WebKitWebPage}
 *
 * @see https://goo.gl/mmoHEx
 * @return TRUE to stop other handlers from being invoked for the event (block the request).
 *         FALSE to continue emission of the event (allow the request).
 */
gboolean
web_page_send_request_cb(WebKitWebPage     *web_page,
						 WebKitURIRequest  *request,
						 WebKitURIResponse *redirected_response,
						 gpointer           user_data) {

	char *request_scheme;
	gchar *request_file_path;
	char *request_file_path_without_query;
	gboolean decision;

	const char *request_uri = webkit_uri_request_get_uri(request);
	request_scheme = g_uri_parse_scheme(request_uri);

	/* NOTE: Returning TRUE blocks the request, while Returning FALSE allows it.
	 * :face_with_rolling_eyes:
	 */
	if (NULL != strstr(request_uri, "mock.js")) {
		/* Never allow mock greeter script to be loaded in the greeter */
		decision = TRUE; /* Blocked */

	} else if (FALSE == secure_mode && NULL != strstr(request_scheme, "http")) {
		decision = FALSE; /* Allowed */

	} else if (0 == strcmp(request_scheme, "data") || 0 == strcmp(request_scheme, "resource")) {
		decision = FALSE; /* Allowed */

	} else if (0 == strcmp(request_scheme, "file")) {
		request_file_path = g_filename_from_uri(request_uri, NULL, NULL);
		request_file_path_without_query = g_strdup(request_file_path);

		request_file_path_without_query = remove_query_and_hash(request_file_path_without_query);

		decision = should_block_request(request_file_path_without_query);

	} else {
		/* In order to ensure the user's privacy & security, only local requests are allowed. */
		g_warning("request scheme error: %s", request_scheme);
		decision = TRUE; /* Blocked */
	}

	g_free(request_scheme);

	return decision;
}


void
web_page_console_message_sent_cb(WebKitWebPage        *web_page,
								 WebKitConsoleMessage *console_message,
								 gpointer              user_data) {
	WebKitDOMDOMWindow *dom_window;
	WebKitDOMDocument *dom_document;
	const gchar *msg_text;
	gboolean is_error;

	msg_text = webkit_console_message_get_text(console_message);
	is_error =
		NULL != strstr(msg_text, "Uncaught") ||
		NULL != strstr(msg_text, "Error") ||
		NULL != strstr(msg_text, "error") ||
		NULL != strstr(msg_text, "Exception") ||
		NULL != strstr(msg_text, "exception") ||
		NULL != strstr(msg_text, "ERROR");

	if (! is_error) {
		return;
	}

	/* This is an Uncaught JS Exception. Notify the UI process so it can show recovery dialog */
	dom_document = webkit_web_page_get_dom_document(web_page);
	dom_window = webkit_dom_document_get_default_view(dom_document);

	if (dom_window) {
		webkit_dom_dom_window_webkit_message_handlers_post_message(
			dom_window, "GreeterBridge", "JavaScriptException"
		);
	}
}


void
page_created_cb(WebKitWebExtension *extension,
				WebKitWebPage      *web_page,
				gpointer            user_data) {

	// save the page_id (global variable)
	page_id = webkit_web_page_get_id(web_page);

	g_signal_connect(web_page, "send-request", G_CALLBACK(web_page_send_request_cb), NULL);

	if (TRUE == detect_theme_errors) {
		g_signal_connect(web_page, "console-message-sent", G_CALLBACK(web_page_console_message_sent_cb), NULL);
	}
}


G_MODULE_EXPORT void
webkit_web_extension_initialize(WebKitWebExtension *extension) {
	LightDMGreeter *greeter = lightdm_greeter_new();
	GError *err = NULL;

	WEB_EXTENSION = extension;
	SESSION_STARTING = FALSE;

	/* load greeter settings from config file */
	keyfile = g_key_file_new();

	g_key_file_load_from_file(
		keyfile,
		CONFIG_DIR "/lightdm-webkit2-greeter.conf",
		G_KEY_FILE_NONE,
		NULL
	);

	secure_mode = get_config_option_as_bool("greeter", "secure_mode", &err);
	if (NULL != err) {
		// Use default value
		secure_mode = TRUE;
		g_clear_error(&err);
	}

	detect_theme_errors = get_config_option_as_bool("greeter", "detect_theme_errors", &err);
	if (NULL != err) {
		// Use default value
		detect_theme_errors = TRUE;
		g_error_free(err);
	}

	paths = g_slist_prepend(paths, THEME_DIR);

	background_images_dir = get_config_option_as_string("branding", "background_images");
	paths = g_slist_prepend(paths, background_images_dir);

	user_image = get_config_option_as_string("branding", "user_image");
	paths = g_slist_prepend(paths, user_image);

	logo = get_config_option_as_string("branding", "logo");
	paths = g_slist_prepend(paths, logo);

	g_signal_connect(
		G_OBJECT(greeter),
		"authentication-complete",
		G_CALLBACK(authentication_complete_cb),
		extension
	);

	g_signal_connect(
		G_OBJECT(greeter),
		"autologin-timer-expired",
		G_CALLBACK(autologin_timer_expired_cb),
		extension
	);

	g_signal_connect(
		G_OBJECT(extension),
		"page-created",
		G_CALLBACK(page_created_cb),
		NULL
	);

	g_signal_connect(
		webkit_script_world_get_default(),
		"window-object-cleared",
		G_CALLBACK(window_object_cleared_callback),
		greeter
	);

	g_signal_connect(
		G_OBJECT(greeter),
		"show-prompt",
		G_CALLBACK(show_prompt_cb),
		extension
	);

	g_signal_connect(
		G_OBJECT(greeter),
		"show-message",
		G_CALLBACK(show_message_cb),
		extension
	);

	/* TODO: This function was deprecated in lightdm 1.11.x.
	 * New function is lightdm_greeter_connect_to_daemon_sync
	 * Wait until it makes it into Debian Stable before making the change.
	 */
	lightdm_greeter_connect_sync(greeter, NULL);
}

/* vim: set ts=4 sw=4 tw=0 noet : */
