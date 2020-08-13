#include "com_artifex_gsjava_GSAPI.h"

#include <iapi.h>
#include <gdevdsp.h>
#include <string.h>
#include <memory>

#include "jni_util.h"
#include "callbacks.h"

using namespace util;

static bool isParamOkay(jobject object, gs_set_param_type type);
static void *getAsPointer(JNIEnv *env, jobject object, gs_set_param_type type, bool *success);

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1revision
	(JNIEnv *env, jclass, jobject revision, jint len)
{
	if (revision == NULL)
		return throwNullPointerException(env, "Revision object is NULL");
	gsapi_revision_t gsrevision;
	jint code = gsapi_revision(&gsrevision, sizeof(gsapi_revision_t));
	if (code == 0)
	{
		setByteArrayField(env, revision, "product", gsrevision.product);
		setByteArrayField(env, revision, "copyright", gsrevision.copyright);
		setLongField(env, revision, "revision", gsrevision.revision);
		setLongField(env, revision, "revisionDate", gsrevision.revisiondate);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1new_1instance
	(JNIEnv *env, jclass, jobject instance, jlong callerHandle)
{
	if (instance == NULL)
		return throwNullPointerException(env, "LongReference object is NULL");

	void *gsInstance;
	int code = gsapi_new_instance(&gsInstance, (void *)callerHandle);
	if (code == 0)
		setLongField(env, instance, "value", (jlong)gsInstance);
	return code;
}

JNIEXPORT void JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1delete_1instance
	(JNIEnv *, jclass, jlong instance)
{
	gsapi_delete_instance((void *)instance);
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1stdio_1with_1handle
	(JNIEnv *env, jclass, jlong instance, jobject stdIn, jobject stdOut, jobject stdErr, jlong callerHandle)
{
	int code = gsapi_set_stdio_with_handle((void *)instance, callbacks::stdInFunction,
		callbacks::stdOutFunction, callbacks::stdErrFunction, (void *)callerHandle);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setIOCallbacks(stdIn, stdOut, stdErr);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1stdio
	(JNIEnv *env, jclass, jlong instance, jobject stdIn, jobject stdOut, jobject stdErr)
{
	int code = gsapi_set_stdio((void *)instance, callbacks::stdInFunction,
		callbacks::stdOutFunction, callbacks::stdErrFunction);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setIOCallbacks(stdIn, stdOut, stdErr);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1poll_1with_1handle
	(JNIEnv *env, jclass, jlong instance, jobject poll, jlong callerHandle)
{
	int code = gsapi_set_poll_with_handle((void *)instance, callbacks::pollFunction, (void *)callerHandle);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setPollCallback(poll);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1poll
	(JNIEnv *env, jclass, jlong instance, jobject poll)
{
	int code = gsapi_set_poll((void *)instance, callbacks::pollFunction);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setPollCallback(poll);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1display_1callback
	(JNIEnv *env, jclass, jlong instance, jobject displayCallback)
{
	display_callback *cb = new display_callback;
	cb->size = sizeof(display_callback);
	cb->version_major = DISPLAY_VERSION_MAJOR;
	cb->version_minor = DISPLAY_VERSION_MINOR;

	cb->display_open = callbacks::display::displayOpenFunction;
	cb->display_preclose = callbacks::display::displayPrecloseFunction;
	cb->display_close = callbacks::display::displayCloseFunction;
	cb->display_presize = callbacks::display::displayPresizeFunction;
	cb->display_size = callbacks::display::displaySizeFunction;
	cb->display_sync = callbacks::display::displaySyncFunction;
	cb->display_page = callbacks::display::displayPageFunction;
	cb->display_update = callbacks::display::displayUpdateFunction;
	cb->display_memalloc = NULL;
	cb->display_memfree = NULL;
	cb->display_separation = callbacks::display::displaySeparationFunction;
	cb->display_adjust_band_height = callbacks::display::displayAdjustBandHeightFunction;
	cb->display_rectangle_request = callbacks::display::displayRectangleRequestFunction;

	int code = gsapi_set_display_callback((void *)instance, cb);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setDisplayCallback(displayCallback);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1register_1callout
	(JNIEnv *env, jclass, jlong instance, jobject callout, jlong calloutHandle)
{
	int code = gsapi_register_callout((void *)instance, callbacks::calloutFunction, (void *)calloutHandle);
	if (code == 0)
	{
		callbacks::setJNIEnv(env);
		callbacks::setCalloutCallback(callout);
	}
	return code;
}

JNIEXPORT void JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1deregister_1callout
	(JNIEnv *env, jclass, jlong instance, jobject callout, jlong calloutHandle)
{
	gsapi_deregister_callout((void *)instance, callbacks::calloutFunction, (void *)calloutHandle);
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1arg_1encoding
	(JNIEnv *env, jclass, jlong instance, jint encoding)
{
	return gsapi_set_arg_encoding((void *)instance, encoding);
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1default_1device_1list
	(JNIEnv *env, jclass, jlong instance, jbyteArray list, jint listlen)
{
	if (list == NULL)
		return throwNullPointerException(env, "list");
	jboolean isCopy = false;
	int code = gsapi_set_default_device_list((void *)instance,
		(const char *)env->GetByteArrayElements(list, &isCopy), listlen);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1get_1default_1device_1list
	(JNIEnv *env, jclass, jlong instance, jobject list, jobject listlen)
{
	char *clist = NULL;
	int clistlen = 0;
	int code = gsapi_get_default_device_list((void *)instance, &clist, &clistlen);
	if (code == 0)
	{
		if (list)
			ByteArrayReference::setValueField(env, list, clist);

		if (listlen)
			IntReference::setValueField(env, listlen, clistlen);
	}
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1init_1with_1args
	(JNIEnv *env, jclass, jlong instance, jint argc, jobjectArray argv)
{
	if (argv == NULL)
		return throwNullPointerException(env, "argv");
	char **cargv = jbyteArray2DToCharArray(env, argv);
	int code = gsapi_init_with_args((void *)instance, argc, cargv);
	delete2DByteArray(argc, cargv);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1string_1begin
	(JNIEnv *env, jclass, jlong instance, jint userErrors, jobject pExitCode)
{
	int exitCode;
	int code = gsapi_run_string_begin((void *)instance, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1string_1continue
	(JNIEnv *env, jclass, jlong instance, jbyteArray str, jint length, jint userErrors, jobject pExitCode)
{
	if (str == NULL)
		return throwNullPointerException(env, "str");
	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(str, &copy);
	int code = gsapi_run_string_continue((void *)instance, cstring, length, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1string_1end
	(JNIEnv *env, jclass, jlong instance, jint userErrors, jobject pExitCode)
{
	int exitCode;
	int code = gsapi_run_string_end((void *)instance, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1string_1with_1length
	(JNIEnv *env, jclass, jlong instance, jbyteArray str, jint length, jint userErrors, jobject pExitCode)
{
	if (str == NULL)
		return throwNullPointerException(env, "str");
	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(str, &copy);
	int code = gsapi_run_string_with_length((void *)instance, cstring, length, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1string
	(JNIEnv *env, jclass, jlong instance, jbyteArray str, jint userErrors, jobject pExitCode)
{
	if (str == NULL)
		return throwNullPointerException(env, "str");
	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(str, &copy);
	int code = gsapi_run_string((void *)instance, cstring, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1run_1file
	(JNIEnv *env, jclass, jlong instance, jbyteArray fileName, jint userErrors, jobject pExitCode)
{
	if (fileName == NULL)
		return throwNullPointerException(env, "fileName");
	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(fileName, &copy);
	int code = gsapi_run_file((void *)instance, cstring, userErrors, &exitCode);
	if (pExitCode)
		IntReference::setValueField(env, pExitCode, exitCode);
	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1exit
	(JNIEnv *env, jclass, jlong instance)
{
	return gsapi_exit((void *)instance);
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1set_1param
	(JNIEnv *env, jclass, jlong instance, jbyteArray param, jobject value, jint paramType)
{
	if (!param)
	{
		throwNullPointerException(env, "param");
		return -1;
	}

	gs_set_param_type type = (gs_set_param_type)paramType;
	bool paramSuccess;
	void *data = getAsPointer(env, value, type, &paramSuccess);
	if (!paramSuccess)
	{
		throwIllegalArgumentException(env, "paramType");
		return -1;
	}

	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(param, &copy);

	int code = gsapi_set_param((void *)instance, cstring, data, type);
	free(data);

	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1get_1param
	(JNIEnv *env, jclass, jlong instance, jbyteArray param, jlong value, jint paramType)
{
	if (!param)
	{
		throwNullPointerException(env, "paramType");
		return -1;
	}

	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(param, &copy);

	int ret = gsapi_get_param((void *)instance, cstring, (void *)value, (gs_set_param_type)paramType);

	return ret;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1get_1param_1once
	(JNIEnv *env, jclass, jlong instance, jbyteArray param, jobject value, jint paramType)
{
	jboolean copy = false;
	int exitCode;
	const char *cstring = (const char *)env->GetByteArrayElements(param, &copy);

	int bytes = gsapi_get_param((void *)instance, cstring, NULL, (gs_set_param_type)paramType);
	if (bytes < 0)
		return bytes;

	void *data = new char[bytes];
	int code = gsapi_get_param((void *)instance, cstring, data, (gs_set_param_type)paramType);
	if (code < 0)
	{
		delete[] data;
		return code;
	}

	int stripped = paramType & ~(gs_spt_more_to_come);
	Reference ref = Reference(env, value);

	jbyteArray arr = NULL;
	const char *str = NULL;
	int len = 0;
	switch (stripped)
	{
	case gs_spt_null:
		break;
	case gs_spt_bool:
		ref.set((jboolean)*((int *)data));
		break;
	case gs_spt_int:
		ref.set(*((jint *)data));
		break;
	case gs_spt_float:
		ref.set(*((jfloat *)data));
		break;
	case gs_spt_long:
		ref.set(*((jlong *)data));
		break;
	case gs_spt_i64:
		ref.set((jlong)*((long long *)data));
		break;
	case gs_spt_size_t:
		ref.set((jlong)*((size_t *)data));
		break;
	case gs_spt_name:
	case gs_spt_string:
	case gs_spt_parsed:
		str = (const char *)data;
		len = strlen(str) + 1;
		arr = env->NewByteArray(len);
		env->SetByteArrayRegion(arr, 0, len, (const jbyte *)str);
		ref.set(arr);
		break;
	case gs_spt_invalid:
	default:
		throwIllegalArgumentException(env, "paramType");
		delete[] data;
		return -1;
		break;
	}
	delete[] data;
	return 0;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1enumerate_1params
   (JNIEnv *env, jclass, jlong instance, jobject iter, jobject key, jobject paramType)
{
	if (!iter)
	{
		throwNullPointerException(env, "iterator is NULL");
		return -1;
	}

	Reference iterRef = Reference(env, iter);

	Reference keyRef = Reference(env, key);
	Reference typeRef = Reference(env, paramType);

	void *citer = (void *)iterRef.longValue();

	if (env->ExceptionCheck())
		return -1;

	const char *ckey;
	gs_set_param_type type;

	int code = gsapi_enumerate_params((void *)instance, &citer, &ckey, &type);

	if (code == 0)
	{
		iterRef.set((jlong)citer);

		jsize len = strlen(ckey) + 1;
		jbyteArray arr = env->NewByteArray(len);
		env->SetByteArrayRegion(arr, 0, len, (const jbyte *)ckey);
		keyRef.set(arr);
		env->DeleteLocalRef(arr);

		typeRef.set((jint)type);
	}

	return code;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1add_1control_1path
	(JNIEnv *env, jclass, jlong instance, jint type, jbyteArray path)
{
	if (!path)
	{
		throwNullPointerException(env, "path is NULL");
		return -1;
	}

	jboolean copy = false;
	const char *cstring = (const char *)env->GetByteArrayElements(path, &copy);

	int exitCode = gsapi_add_control_path((void *)instance, type, cstring);

	return exitCode;
}

JNIEXPORT jint JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1remove_1control_1path
(JNIEnv *env, jclass, jlong instance, jint type, jbyteArray path)
{
	if (!path)
	{
		throwNullPointerException(env, "path is NULL");
		return -1;
	}

	jboolean copy = false;
	const char *cstring = (const char *)env->GetByteArrayElements(path, &copy);

	int exitCode = gsapi_remove_control_path((void *)instance, type, cstring);

	return exitCode;
}

JNIEXPORT void JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1purge_1control_1paths
	(JNIEnv *, jclass, jlong instance, jint type)
{
	gsapi_purge_control_paths((void *)instance, type);
}

JNIEXPORT void JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1activate_1path_1control
	(JNIEnv *, jclass, jlong instance, jboolean enable)
{
	gsapi_activate_path_control((void *)instance, enable);
}

JNIEXPORT jboolean JNICALL Java_com_artifex_gsjava_GSAPI_gsapi_1is_1path_1control_1active
	(JNIEnv *env, jclass, jlong instance)
{
	return gsapi_is_path_control_active((void *)instance);
}

void *getAsPointer(JNIEnv *env, jobject object, gs_set_param_type type, bool *success)
{
	*success = true;
	void *result = NULL;
	int stripped = type & ~gs_spt_more_to_come;

	jbyteArray arr = NULL;
	jboolean copy = false;
	const char *cstring = NULL;
	jsize len = 0;
	switch (stripped)
	{
	case gs_spt_null:
		return result;
		break;
	case gs_spt_bool:
		result = malloc(sizeof(int));
		*((int *)result) = (bool)toBoolean(env, object);
		break;
	case gs_spt_int:
		result = malloc(sizeof(int));
		*((int *)result) = (int)toInt(env, object);
		break;
	case gs_spt_float:
		result = malloc(sizeof(float));
		*((float *)result) = (float)toFloat(env, object);
		break;
	case gs_spt_long:
	case gs_spt_i64:
		result = malloc(sizeof(long long));
		*((long long *)result) = (long long)toLong(env, object);
		break;
	case gs_spt_size_t:
		result = malloc(sizeof(size_t));
		*((size_t *)result) = (size_t)toLong(env, object);
		break;
	case gs_spt_name:
	case gs_spt_string:
	case gs_spt_parsed:
		arr = (jbyteArray)object;
		cstring = (const char *)env->GetByteArrayElements(arr, &copy);
		len = env->GetArrayLength(arr);
		result = malloc(sizeof(char) * len);
		((char *)result)[len - 1] = 0;
		break;
	case gs_spt_invalid:
	default:
		*success = false;
		break;
	}
	if (env->ExceptionCheck())
	{
		if (result)
			free(result);
		result = NULL;
		*success = false;
	}
	return result;
}
