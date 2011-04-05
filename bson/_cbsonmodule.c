/*
 * Copyright 2009-2010 10gen, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains C implementations of some of the functions
 * needed by the bson module. If possible, these implementations
 * should be used to speed up BSON encoding and decoding.
 */

#include <Python.h>
#include <stdio.h>

#include <datetime.h>

#include "_cbson.h"
#include "buffer.h"
#include "time64.h"
#include "encoding_helpers.h"

static PyObject* Binary = NULL;
static PyObject* Code = NULL;
static PyObject* ObjectId = NULL;
static PyObject* DBRef = NULL;
static PyObject* RECompile = NULL;
static PyObject* UUID = NULL;
static PyObject* Timestamp = NULL;
static PyObject* MinKey = NULL;
static PyObject* MaxKey = NULL;
static PyObject* UTC = NULL;
static PyTypeObject* REType = NULL;

#if PY_VERSION_HEX < 0x02050000
#define WARN(category, message)                 \
    PyErr_Warn((category), (message))
#else
#define WARN(category, message)                 \
    PyErr_WarnEx((category), (message), 1)
#endif

/* Maximum number of regex flags */
#define FLAGS_SIZE 7

#if defined(WIN32) || defined(_MSC_VER)
/* This macro is basically an implementation of asprintf for win32
 * We get the length of the int as string and malloc a buffer for it,
 * returning -1 if that malloc fails. We then actually print to the
 * buffer to get the string value as an int. Like asprintf, the result
 * must be explicitly free'd when done being used.
 */
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#define INT2STRING(buffer, i)                                           \
    *(buffer) = malloc(_scprintf("%d", (i)) + 1),                       \
        (!(buffer) ?                                                    \
         -1 :                                                           \
         _snprintf_s(*(buffer),                                         \
                     _scprintf("%d", (i)) + 1,                          \
                     _scprintf("%d", (i)) + 1,                          \
                     "%d",                                              \
                     (i)))
#define STRCAT(dest, n, src) strcat_s((dest), (n), (src))
#else
#define INT2STRING(buffer, i)                                           \
    *(buffer) = malloc(_scprintf("%d", (i)) + 1),                       \
        (!(buffer) ?                                                    \
         -1 :                                                           \
         _snprintf(*(buffer),                                           \
                     _scprintf("%d", (i)) + 1,                          \
                     "%d",                                              \
                     (i)))
#define STRCAT(dest, n, src) strcat((dest), (src))
#endif
#else
#define INT2STRING(buffer, i) asprintf((buffer), "%d", (i))
#define STRCAT(dest, n, src) strcat((dest), (src))
#endif


static PyObject* elements_to_dict(const char* string, int max,
                                  PyObject* as_class, unsigned char tz_aware);

static int _write_element_to_buffer(buffer_t buffer, int type_byte, PyObject* value,
                                    unsigned char check_keys, unsigned char first_attempt);

/* Date stuff */
static PyObject* datetime_from_millis(long long millis) {
    int microseconds = (millis % 1000) * 1000;
    Time64_T seconds = millis / 1000;
    struct TM timeinfo;
    gmtime64_r(&seconds, &timeinfo);

    return PyDateTime_FromDateAndTime(timeinfo.tm_year + 1900,
                                      timeinfo.tm_mon + 1,
                                      timeinfo.tm_mday,
                                      timeinfo.tm_hour,
                                      timeinfo.tm_min,
                                      timeinfo.tm_sec,
                                      microseconds);
}

static long long millis_from_datetime(PyObject* datetime) {
    struct TM timeinfo;
    long long millis;

    timeinfo.tm_year = PyDateTime_GET_YEAR(datetime) - 1900;
    timeinfo.tm_mon = PyDateTime_GET_MONTH(datetime) - 1;
    timeinfo.tm_mday = PyDateTime_GET_DAY(datetime);
    timeinfo.tm_hour = PyDateTime_DATE_GET_HOUR(datetime);
    timeinfo.tm_min = PyDateTime_DATE_GET_MINUTE(datetime);
    timeinfo.tm_sec = PyDateTime_DATE_GET_SECOND(datetime);

    millis = timegm64(&timeinfo) * 1000;
    millis += PyDateTime_DATE_GET_MICROSECOND(datetime) / 1000;
    return millis;
}

/* Just make this compatible w/ the old API. */
int buffer_write_bytes(buffer_t buffer, const char* data, int size) {
    if (buffer_write(buffer, data, size)) {
        PyErr_NoMemory();
        return 0;
    }
    return 1;
}

/* returns 0 on failure */
static int write_string(buffer_t buffer, PyObject* py_string) {
    Py_ssize_t string_length;
    const char* string = PyString_AsString(py_string);
    if (!string) {
        return 1;
    }
    string_length = PyString_Size(py_string) + 1;

    if (!buffer_write_bytes(buffer, (const char*)&string_length, 4)) {
        return 0;
    }
    if (!buffer_write_bytes(buffer, string, string_length)) {
        return 0;
    }
    return 1;
}

/* Get an error class from the bson.errors module.
 *
 * Returns a new ref */
static PyObject* _error(char* name) {
    PyObject* error;
    PyObject* errors = PyImport_ImportModule("bson.errors");
    if (!errors) {
        return NULL;
    }
    error = PyObject_GetAttrString(errors, name);
    Py_DECREF(errors);
    return error;
}

/* Reload a cached Python object.
 *
 * Returns non-zero on failure. */
static int _reload_object(PyObject** object, char* module_name, char* object_name) {
    PyObject* module;

    module = PyImport_ImportModule(module_name);
    if (!module) {
        return 1;
    }

    *object = PyObject_GetAttrString(module, object_name);
    Py_DECREF(module);

    return (*object) ? 0 : 2;
}

/* Reload all cached Python objects.
 *
 * Returns non-zero on failure. */
static int _reload_python_objects(void) {
    if (_reload_object(&Binary, "bson.binary", "Binary") ||
        _reload_object(&Code, "bson.code", "Code") ||
        _reload_object(&ObjectId, "bson.objectid", "ObjectId") ||
        _reload_object(&DBRef, "bson.dbref", "DBRef") ||
        _reload_object(&Timestamp, "bson.timestamp", "Timestamp") ||
        _reload_object(&MinKey, "bson.min_key", "MinKey") ||
        _reload_object(&MaxKey, "bson.max_key", "MaxKey") ||
        _reload_object(&UTC, "bson.tz_util", "utc") ||
        _reload_object(&RECompile, "re", "compile")) {
        return 1;
    }
    /* If we couldn't import uuid then we must be on 2.4. Just ignore. */
    if (_reload_object(&UUID, "uuid", "UUID") == 1) {
        UUID = NULL;
        PyErr_Clear();
    }
    /* Reload our REType hack too. */
    REType = PyObject_CallFunction(RECompile, "O",
                                   PyString_FromString(""))->ob_type;
    return 0;
}

static int write_element_to_buffer(buffer_t buffer, int type_byte,
                                   PyObject* value, unsigned char check_keys,
                                   unsigned char first_attempt) {
    int result;
    if(Py_EnterRecursiveCall(" while encoding an object to BSON "))
        return 0;
    result = _write_element_to_buffer(buffer, type_byte, value,
                                      check_keys, first_attempt);
    Py_LeaveRecursiveCall();
    return result;
}

/* TODO our platform better be little-endian w/ 4-byte ints! */
/* Write a single value to the buffer (also write it's type_byte, for which
 * space has already been reserved.
 *
 * returns 0 on failure */
static int _write_element_to_buffer(buffer_t buffer, int type_byte, PyObject* value,
                                    unsigned char check_keys, unsigned char first_attempt) {
    if (PyBool_Check(value)) {
        const long bool = PyInt_AsLong(value);
        const char c = bool ? 0x01 : 0x00;
        *(buffer_get_buffer(buffer) + type_byte) = 0x08;
        return buffer_write_bytes(buffer, &c, 1);
    }
    else if (PyInt_Check(value)) {
        const long long_value = PyInt_AsLong(value);
        const int int_value = (int)long_value;
        if (PyErr_Occurred() || long_value != int_value) { /* Overflow */
            long long long_long_value;
            PyErr_Clear();
            long_long_value = PyLong_AsLongLong(value);
            if (PyErr_Occurred()) { /* Overflow AGAIN */
                PyErr_SetString(PyExc_OverflowError,
                                "MongoDB can only handle up to 8-byte ints");
                return 0;
            }
            *(buffer_get_buffer(buffer) + type_byte) = 0x12;
            return buffer_write_bytes(buffer, (const char*)&long_long_value, 8);
        }
        *(buffer_get_buffer(buffer) + type_byte) = 0x10;
        return buffer_write_bytes(buffer, (const char*)&int_value, 4);
    } else if (PyLong_Check(value)) {
        const long long long_long_value = PyLong_AsLongLong(value);
        if (PyErr_Occurred()) { /* Overflow */
            PyErr_SetString(PyExc_OverflowError,
                            "MongoDB can only handle up to 8-byte ints");
            return 0;
        }
        *(buffer_get_buffer(buffer) + type_byte) = 0x12;
        return buffer_write_bytes(buffer, (const char*)&long_long_value, 8);
    } else if (PyFloat_Check(value)) {
        const double d = PyFloat_AsDouble(value);
        *(buffer_get_buffer(buffer) + type_byte) = 0x01;
        return buffer_write_bytes(buffer, (const char*)&d, 8);
    } else if (value == Py_None) {
        *(buffer_get_buffer(buffer) + type_byte) = 0x0A;
        return 1;
    } else if (PyDict_Check(value)) {
        *(buffer_get_buffer(buffer) + type_byte) = 0x03;
        return write_dict(buffer, value, check_keys, 0);
    } else if (PyList_Check(value) || PyTuple_Check(value)) {
        int start_position,
            length_location,
            items,
            length,
            i;
        char zero = 0;

        *(buffer_get_buffer(buffer) + type_byte) = 0x04;
        start_position = buffer_get_position(buffer);

        /* save space for length */
        length_location = buffer_save_space(buffer, 4);
        if (length_location == -1) {
            PyErr_NoMemory();
            return 0;
        }

        items = PySequence_Size(value);
        for(i = 0; i < items; i++) {
            int list_type_byte = buffer_save_space(buffer, 1);
            char* name;
            PyObject* item_value;

            if (list_type_byte == -1) {
                PyErr_NoMemory();
                return 0;
            }
            if (INT2STRING(&name, i) < 0 || !name) {
                PyErr_NoMemory();
                return 0;
            }
            if (!buffer_write_bytes(buffer, name, strlen(name) + 1)) {
                free(name);
                return 0;
            }
            free(name);

            item_value = PySequence_GetItem(value, i);
            if (!write_element_to_buffer(buffer, list_type_byte, item_value, check_keys, 1)) {
                Py_DECREF(item_value);
                return 0;
            }
            Py_DECREF(item_value);
        }

        /* write null byte and fill in length */
        if (!buffer_write_bytes(buffer, &zero, 1)) {
            return 0;
        }
        length = buffer_get_position(buffer) - start_position;
        memcpy(buffer_get_buffer(buffer) + length_location, &length, 4);
        return 1;
    } else if (PyObject_IsInstance(value, Binary)) {
        PyObject* subtype_object;

        *(buffer_get_buffer(buffer) + type_byte) = 0x05;
        subtype_object = PyObject_GetAttrString(value, "subtype");
        if (!subtype_object) {
            return 0;
        }
        {
            const long long_subtype = PyInt_AsLong(subtype_object);
            const char subtype = (const char)long_subtype;
            const int length = PyString_Size(value);

            Py_DECREF(subtype_object);
            if (subtype == 2) {
                const int other_length = length + 4;
                if (!buffer_write_bytes(buffer, (const char*)&other_length, 4)) {
                    return 0;
                }
                if (!buffer_write_bytes(buffer, &subtype, 1)) {
                    return 0;
                }
            }
            if (!buffer_write_bytes(buffer, (const char*)&length, 4)) {
                return 0;
            }
            if (subtype != 2) {
                if (!buffer_write_bytes(buffer, &subtype, 1)) {
                    return 0;
                }
            }
            {
                const char* string = PyString_AsString(value);
                if (!string) {
                    return 0;
                }
                if (!buffer_write_bytes(buffer, string, length)) {
                    return 0;
                }
            }
        }
        return 1;
    } else if (UUID && PyObject_IsInstance(value, UUID)) {
        // Just a special case of Binary above, but simpler to do as a separate case

        // UUID is always 16 bytes, subtype 3
        int length = 16;
        const char subtype = 3;

        PyObject* bytes;

        *(buffer_get_buffer(buffer) + type_byte) = 0x05;
        if (!buffer_write_bytes(buffer, (const char*)&length, 4)) {
            return 0;
        }
        if (!buffer_write_bytes(buffer, &subtype, 1)) {
            return 0;
        }

        bytes = PyObject_GetAttrString(value, "bytes");
        if (!bytes) {
            return 0;
        }
        if (!buffer_write_bytes(buffer, PyString_AsString(bytes), length)) {
            Py_DECREF(bytes);
            return 0;
        }
        Py_DECREF(bytes);
        return 1;
    } else if (PyObject_IsInstance(value, Code)) {
        int start_position,
            length_location,
            length;
        PyObject* scope;

        *(buffer_get_buffer(buffer) + type_byte) = 0x0F;

        start_position = buffer_get_position(buffer);
        /* save space for length */
        length_location = buffer_save_space(buffer, 4);
        if (length_location == -1) {
            PyErr_NoMemory();
            return 0;
        }

        if (!write_string(buffer, value)) {
            return 0;
        }

        scope = PyObject_GetAttrString(value, "scope");
        if (!scope) {
            return 0;
        }
        if (!write_dict(buffer, scope, 0, 0)) {
            Py_DECREF(scope);
            return 0;
        }
        Py_DECREF(scope);

        length = buffer_get_position(buffer) - start_position;
        memcpy(buffer_get_buffer(buffer) + length_location, &length, 4);
        return 1;
    } else if (PyString_Check(value)) {
        int result;
        result_t status;

        *(buffer_get_buffer(buffer) + type_byte) = 0x02;
        status = check_string((const unsigned char*)PyString_AsString(value),
                              PyString_Size(value), 1, 0);
        if (status == NOT_UTF_8) {
            PyObject* InvalidStringData = _error("InvalidStringData");
            PyErr_SetString(InvalidStringData,
                            "strings in documents must be valid UTF-8");
            Py_DECREF(InvalidStringData);
            return 0;
        }
        result = write_string(buffer, value);
        return result;
    } else if (PyUnicode_Check(value)) {
        PyObject* encoded;
        int result;

        *(buffer_get_buffer(buffer) + type_byte) = 0x02;
        encoded = PyUnicode_AsUTF8String(value);
        if (!encoded) {
            return 0;
        }
        result = write_string(buffer, encoded);
        Py_DECREF(encoded);
        return result;
    } else if (PyDateTime_Check(value)) {
        long long millis;
        PyObject* utcoffset = PyObject_CallMethod(value, "utcoffset", NULL);
        if (utcoffset != Py_None) {
            PyObject* result = PyNumber_Subtract(value, utcoffset);
            Py_DECREF(utcoffset);
            if (!result) {
                return 0;
            }
            millis = millis_from_datetime(result);
            Py_DECREF(result);
        } else {
            millis = millis_from_datetime(value);
        }
        *(buffer_get_buffer(buffer) + type_byte) = 0x09;
        return buffer_write_bytes(buffer, (const char*)&millis, 8);
    } else if (PyObject_IsInstance(value, ObjectId)) {
        PyObject* pystring = PyObject_GetAttrString(value, "_ObjectId__id");
        if (!pystring) {
            return 0;
        }
        {
            const char* as_string = PyString_AsString(pystring);
            if (!as_string) {
                Py_DECREF(pystring);
                return 0;
            }
            if (!buffer_write_bytes(buffer, as_string, 12)) {
                Py_DECREF(pystring);
                return 0;
            }
            Py_DECREF(pystring);
            *(buffer_get_buffer(buffer) + type_byte) = 0x07;
        }
        return 1;
    } else if (PyObject_IsInstance(value, DBRef)) {
        PyObject* as_doc = PyObject_CallMethod(value, "as_doc", NULL);
        if (!as_doc) {
            return 0;
        }
        if (!write_dict(buffer, as_doc, 0, 0)) {
            Py_DECREF(as_doc);
            return 0;
        }
        Py_DECREF(as_doc);
        *(buffer_get_buffer(buffer) + type_byte) = 0x03;
        return 1;
    } else if (PyObject_IsInstance(value, Timestamp)) {
        PyObject* obj;
        long i;

        obj = PyObject_GetAttrString(value, "inc");
        if (!obj) {
            return 0;
        }
        i = PyInt_AsLong(obj);
        Py_DECREF(obj);
        if (!buffer_write_bytes(buffer, (const char*)&i, 4)) {
            return 0;
        }

        obj = PyObject_GetAttrString(value, "time");
        if (!obj) {
            return 0;
        }
        i = PyInt_AsLong(obj);
        Py_DECREF(obj);
        if (!buffer_write_bytes(buffer, (const char*)&i, 4)) {
            return 0;
        }

        *(buffer_get_buffer(buffer) + type_byte) = 0x11;
        return 1;
    }
    else if (PyObject_TypeCheck(value, REType)) {
        PyObject* py_flags = PyObject_GetAttrString(value, "flags");
        PyObject* py_pattern;
        PyObject* encoded_pattern;
        long int_flags;
        char flags[FLAGS_SIZE];
        char check_utf8 = 0;
        int pattern_length,
            flags_length;
        result_t status;

        if (!py_flags) {
            return 0;
        }
        int_flags = PyInt_AsLong(py_flags);
        Py_DECREF(py_flags);
        py_pattern = PyObject_GetAttrString(value, "pattern");
        if (!py_pattern) {
            return 0;
        }

        if (PyUnicode_Check(py_pattern)) {
            encoded_pattern = PyUnicode_AsUTF8String(py_pattern);
            Py_DECREF(py_pattern);
            if (!encoded_pattern) {
                return 0;
            }
        } else {
            encoded_pattern = py_pattern;
            check_utf8 = 1;
        }

        status = check_string((const unsigned char*)PyString_AsString(encoded_pattern),
                              PyString_Size(encoded_pattern), check_utf8, 1);
        if (status == NOT_UTF_8) {
            PyObject* InvalidStringData = _error("InvalidStringData");
            PyErr_SetString(InvalidStringData,
                            "regex patterns must be valid UTF-8");
            Py_DECREF(InvalidStringData);
            return 0;
        } else if (status == HAS_NULL) {
            PyObject* InvalidDocument = _error("InvalidDocument");
            PyErr_SetString(InvalidDocument,
                            "regex patterns must not contain the NULL byte");
            Py_DECREF(InvalidDocument);
            return 0;
        }

        {
            const char* pattern =  PyString_AsString(encoded_pattern);
            pattern_length = strlen(pattern) + 1;

            if (!buffer_write_bytes(buffer, pattern, pattern_length)) {
                Py_DECREF(encoded_pattern);
                return 0;
            }
        }
        Py_DECREF(encoded_pattern);

        flags[0] = 0;
        /* TODO don't hardcode these */
        if (int_flags & 2) {
            STRCAT(flags, FLAGS_SIZE, "i");
        }
        if (int_flags & 4) {
            STRCAT(flags, FLAGS_SIZE, "l");
        }
        if (int_flags & 8) {
            STRCAT(flags, FLAGS_SIZE, "m");
        }
        if (int_flags & 16) {
            STRCAT(flags, FLAGS_SIZE, "s");
        }
        if (int_flags & 32) {
            STRCAT(flags, FLAGS_SIZE, "u");
        }
        if (int_flags & 64) {
            STRCAT(flags, FLAGS_SIZE, "x");
        }
        flags_length = strlen(flags) + 1;
        if (!buffer_write_bytes(buffer, flags, flags_length)) {
            return 0;
        }
        *(buffer_get_buffer(buffer) + type_byte) = 0x0B;
        return 1;
    } else if (PyObject_IsInstance(value, MinKey)) {
        *(buffer_get_buffer(buffer) + type_byte) = 0xFF;
        return 1;
    } else if (PyObject_IsInstance(value, MaxKey)) {
        *(buffer_get_buffer(buffer) + type_byte) = 0x7F;
        return 1;
    } else if (first_attempt) {
        /* Try reloading the modules and having one more go at it. */
        if (WARN(PyExc_RuntimeWarning, "couldn't encode - reloading python "
                 "modules and trying again. if you see this without getting "
                 "an InvalidDocument exception please see http://api.mongodb"
                 ".org/python/current/faq.html#does-pymongo-work-with-mod-"
                 "wsgi") == -1) {
            return 0;
        }
        if (_reload_python_objects()) {
            return 0;
        }
        return write_element_to_buffer(buffer, type_byte, value, check_keys, 0);
    }
    {
        PyObject* errmsg = PyString_FromString("Cannot encode object: ");
        PyObject* repr = PyObject_Repr(value);
        PyObject* InvalidDocument = _error("InvalidDocument");
        PyString_ConcatAndDel(&errmsg, repr);
        PyErr_SetString(InvalidDocument, PyString_AsString(errmsg));
        Py_DECREF(errmsg);
        Py_DECREF(InvalidDocument);
        return 0;
    }
}

static int check_key_name(const char* name,
                          const Py_ssize_t name_length) {
    int i;
    if (name_length > 0 && name[0] == '$') {
        PyObject* InvalidDocument = _error("InvalidDocument");
        PyObject* errmsg = PyString_FromFormat("key '%s' must not start with '$'", name);
        PyErr_SetString(InvalidDocument, PyString_AsString(errmsg));
        Py_DECREF(errmsg);
        Py_DECREF(InvalidDocument);
        return 0;
    }
    for (i = 0; i < name_length; i++) {
        if (name[i] == '.') {
            PyObject* InvalidDocument = _error("InvalidDocument");
            PyObject* errmsg = PyString_FromFormat("key '%s' must not contain '.'", name);
            PyErr_SetString(InvalidDocument, PyString_AsString(errmsg));
            Py_DECREF(errmsg);
            Py_DECREF(InvalidDocument);
            return 0;
        }
    }
    return 1;
}

/* Write a (key, value) pair to the buffer.
 *
 * Returns 0 on failure */
int write_pair(buffer_t buffer, const char* name, Py_ssize_t name_length,
               PyObject* value, unsigned char check_keys, unsigned char allow_id) {
    int type_byte;

    /* Don't write any _id elements unless we're explicitly told to -
     * _id has to be written first so we do so, but don't bother
     * deleting it from the dictionary being written. */
    if (!allow_id && strcmp(name, "_id") == 0) {
        return 1;
    }

    type_byte = buffer_save_space(buffer, 1);
    if (type_byte == -1) {
        PyErr_NoMemory();
        return 0;
    }
    if (check_keys && !check_key_name(name, name_length)) {
        return 0;
    }
    if (!buffer_write_bytes(buffer, name, name_length + 1)) {
        return 0;
    }
    if (!write_element_to_buffer(buffer, type_byte, value, check_keys, 1)) {
        return 0;
    }
    return 1;
}

int decode_and_write_pair(buffer_t buffer,
                          PyObject* key, PyObject* value,
                          unsigned char check_keys, unsigned char top_level) {
    PyObject* encoded;
    if (PyUnicode_Check(key)) {
        result_t status;
        encoded = PyUnicode_AsUTF8String(key);
        if (!encoded) {
            return 0;
        }
        status = check_string((const unsigned char*)PyString_AsString(encoded),
                              PyString_Size(encoded), 0, 1);

        if (status == HAS_NULL) {
            PyObject* InvalidDocument = _error("InvalidDocument");
            PyErr_SetString(InvalidDocument,
                            "Key names must not contain the NULL byte");
            Py_DECREF(InvalidDocument);
            return 0;
        }
    } else if (PyString_Check(key)) {
        result_t status;
        encoded = key;
        Py_INCREF(encoded);

        status = check_string((const unsigned char*)PyString_AsString(encoded),
                                       PyString_Size(encoded), 1, 1);

        if (status == NOT_UTF_8) {
            PyObject* InvalidStringData = _error("InvalidStringData");
            PyErr_SetString(InvalidStringData,
                            "strings in documents must be valid UTF-8");
            Py_DECREF(InvalidStringData);
            return 0;
        } else if (status == HAS_NULL) {
            PyObject* InvalidDocument = _error("InvalidDocument");
            PyErr_SetString(InvalidDocument,
                            "Key names must not contain the NULL byte");
            Py_DECREF(InvalidDocument);
            return 0;
        }
    } else {
        PyObject* InvalidDocument = _error("InvalidDocument");
        PyObject* errmsg = PyString_FromString("documents must have only string keys, key was ");
        PyObject* repr = PyObject_Repr(key);
        PyString_ConcatAndDel(&errmsg, repr);
        PyErr_SetString(InvalidDocument, PyString_AsString(errmsg));
        Py_DECREF(InvalidDocument);
        Py_DECREF(errmsg);
        return 0;
    }

    /* If top_level is True, don't allow writing _id here - it was already written. */
    if (!write_pair(buffer, PyString_AsString(encoded),
                    PyString_Size(encoded), value, check_keys, !top_level)) {
        Py_DECREF(encoded);
        return 0;
    }

    Py_DECREF(encoded);
    return 1;
}

/* returns 0 on failure */
int write_dict(buffer_t buffer, PyObject* dict, unsigned char check_keys, unsigned char top_level) {
    PyObject* key;
    PyObject* iter;
    char zero = 0;
    int length;
    int length_location;

    if (!PyDict_Check(dict)) {
        PyObject* errmsg = PyString_FromString("encoder expected a mapping type but got: ");
        PyObject* repr = PyObject_Repr(dict);
        PyString_ConcatAndDel(&errmsg, repr);
        PyErr_SetString(PyExc_TypeError, PyString_AsString(errmsg));
        Py_DECREF(errmsg);
        return 0;
    }

    length_location = buffer_save_space(buffer, 4);
    if (length_location == -1) {
        PyErr_NoMemory();
        return 0;
    }

    /* Write _id first if this is a top level doc. */
    if (top_level) {
        PyObject* _id = PyDict_GetItemString(dict, "_id");
        if (_id) {
            /* Don't bother checking keys, but do make sure we're allowed to
             * write _id */
            if (!write_pair(buffer, "_id", 3, _id, 0, 1)) {
                return 0;
            }
        }
    }

    iter = PyObject_GetIter(dict);
    if (iter == NULL) {
        return 0;
    }
    while ((key = PyIter_Next(iter)) != NULL) {
        PyObject* value = PyDict_GetItem(dict, key);
        if (!value) {
            PyErr_SetObject(PyExc_KeyError, key);
            Py_DECREF(key);
            Py_DECREF(iter);
            return 0;
        }
        if (!decode_and_write_pair(buffer, key, value, check_keys, top_level)) {
            Py_DECREF(key);
            Py_DECREF(iter);
            return 0;
        }
        Py_DECREF(key);
    }
    Py_DECREF(iter);

    /* write null byte and fill in length */
    if (!buffer_write_bytes(buffer, &zero, 1)) {
        return 0;
    }
    length = buffer_get_position(buffer) - length_location;
    memcpy(buffer_get_buffer(buffer) + length_location, &length, 4);
    return 1;
}

static PyObject* _cbson_dict_to_bson(PyObject* self, PyObject* args) {
    PyObject* dict;
    PyObject* result;
    unsigned char check_keys;
    buffer_t buffer;

    if (!PyArg_ParseTuple(args, "Ob", &dict, &check_keys)) {
        return NULL;
    }

    buffer = buffer_new();
    if (!buffer) {
        PyErr_NoMemory();
        return NULL;
    }

    if (!write_dict(buffer, dict, check_keys, 1)) {
        buffer_free(buffer);
        return NULL;
    }

    /* objectify buffer */
    result = Py_BuildValue("s#", buffer_get_buffer(buffer),
                           buffer_get_position(buffer));
    buffer_free(buffer);
    return result;
}

static PyObject* get_value(const char* buffer, int* position, int type,
                           PyObject* as_class, unsigned char tz_aware) {
    PyObject* value;
    switch (type) {
    case 1:
        {
            double d;
            memcpy(&d, buffer + *position, 8);
            value = PyFloat_FromDouble(d);
            if (!value) {
                return NULL;
            }
            *position += 8;
            break;
        }
    case 2:
    case 13:
    case 14:
        {
            int value_length = ((int*)(buffer + *position))[0] - 1;
            *position += 4;
            value = PyUnicode_DecodeUTF8(buffer + *position, value_length, "strict");
            if (!value) {
                return NULL;
            }
            *position += value_length + 1;
            break;
        }
    case 3:
        {
            int size;
            memcpy(&size, buffer + *position, 4);
            value = elements_to_dict(buffer + *position + 4, size - 5, as_class, tz_aware);
            if (!value) {
                return NULL;
            }

            /* Decoding for DBRefs */
            if (strcmp(buffer + *position + 5, "$ref") == 0) { /* DBRef */
                PyObject* dbref;
                PyObject* collection = PyDict_GetItemString(value, "$ref");
                PyObject* id = PyDict_GetItemString(value, "$id");
                PyObject* database = PyDict_GetItemString(value, "$db");

                Py_INCREF(collection);
                PyDict_DelItemString(value, "$ref");
                Py_INCREF(id);
                PyDict_DelItemString(value, "$id");

                if (database == NULL) {
                    database = Py_None;
                    Py_INCREF(database);
                } else {
                    Py_INCREF(database);
                    PyDict_DelItemString(value, "$db");
                }

                dbref = PyObject_CallFunctionObjArgs(DBRef, collection, id, database, value, NULL);
                Py_DECREF(value);
                value = dbref;

                Py_DECREF(id);
                Py_DECREF(collection);
                Py_DECREF(database);
                if (!value) {
                    return NULL;
                }
            }

            *position += size;
            break;
        }
    case 4:
        {
            int size,
                end;

            memcpy(&size, buffer + *position, 4);
            end = *position + size - 1;
            *position += 4;

            value = PyList_New(0);
            if (!value) {
                return NULL;
            }
            while (*position < end) {
                PyObject* to_append;

                int type = (int)buffer[(*position)++];
                int key_size = strlen(buffer + *position);
                *position += key_size + 1; /* just skip the key, they're in order. */
                to_append = get_value(buffer, position, type, as_class, tz_aware);
                if (!to_append) {
                    return NULL;
                }
                PyList_Append(value, to_append);
                Py_DECREF(to_append);
            }
            (*position)++;
            break;
        }
    case 5:
        {
            PyObject* data;
            PyObject* st;
            int length,
                subtype;

            memcpy(&length, buffer + *position, 4);
            subtype = (unsigned char)buffer[*position + 4];

            if (subtype == 2) {
                data = PyString_FromStringAndSize(buffer + *position + 9, length - 4);
            } else {
                data = PyString_FromStringAndSize(buffer + *position + 5, length);
            }
            if (!data) {
                return NULL;
            }

            if (subtype == 3 && UUID) { // Encode as UUID, not Binary
                PyObject* kwargs;
                PyObject* args = PyTuple_New(0);
                if (!args) {
                    return NULL;
                }
                kwargs = PyDict_New();
                if (!kwargs) {
                    Py_DECREF(args);
                    return NULL;
                }

                assert(length == 16); // UUID should always be 16 bytes

                PyDict_SetItemString(kwargs, "bytes", data);
                value = PyObject_Call(UUID, args, kwargs);

                Py_DECREF(args);
                Py_DECREF(kwargs);
                Py_DECREF(data);
                if (!value) {
                    return NULL;
                }

                *position += length + 5;
                break;
            }

            st = PyInt_FromLong(subtype);
            if (!st) {
                Py_DECREF(data);
                return NULL;
            }
            value = PyObject_CallFunctionObjArgs(Binary, data, st, NULL);
            Py_DECREF(st);
            Py_DECREF(data);
            if (!value) {
                return NULL;
            }
            *position += length + 5;
            break;
        }
    case 6:
    case 10:
        {
            value = Py_None;
            Py_INCREF(value);
            break;
        }
    case 7:
        {
            value = PyObject_CallFunction(ObjectId, "s#", buffer + *position, 12);
            if (!value) {
                return NULL;
            }
            *position += 12;
            break;
        }
    case 8:
        {
            value = buffer[(*position)++] ? Py_True : Py_False;
            Py_INCREF(value);
            break;
        }
    case 9:
        {
            PyObject* replace;
            PyObject* args;
            PyObject* kwargs;
            PyObject* naive = datetime_from_millis(*(long long*)(buffer + *position));
            *position += 8;
            if (!tz_aware) { /* In the naive case, we're done here. */
                value = naive;
                break;
            }

            if (!naive) {
                return NULL;
            }
            replace = PyObject_GetAttrString(naive, "replace");
            Py_DECREF(naive);
            if (!replace) {
                return NULL;
            }
            args = PyTuple_New(0);
            if (!args) {
                Py_DECREF(replace);
                return NULL;
            }
            kwargs = PyDict_New();
            if (!kwargs) {
                Py_DECREF(replace);
                Py_DECREF(args);
                return NULL;
            }
            if (PyDict_SetItemString(kwargs, "tzinfo", UTC) == -1) {
                Py_DECREF(replace);
                Py_DECREF(args);
                Py_DECREF(kwargs);
                return NULL;
            }
            value = PyObject_Call(replace, args, kwargs);
            Py_DECREF(replace);
            Py_DECREF(args);
            Py_DECREF(kwargs);
            break;
        }
    case 11:
        {
            int flags_length,
                flags,
                i;

            int pattern_length = strlen(buffer + *position);
            PyObject* pattern = PyUnicode_DecodeUTF8(buffer + *position, pattern_length, "strict");
            if (!pattern) {
                return NULL;
            }
            *position += pattern_length + 1;
            flags_length = strlen(buffer + *position);
            flags = 0;
            for (i = 0; i < flags_length; i++) {
                if (buffer[*position + i] == 'i') {
                    flags |= 2;
                } else if (buffer[*position + i] == 'l') {
                    flags |= 4;
                } else if (buffer[*position + i] == 'm') {
                    flags |= 8;
                } else if (buffer[*position + i] == 's') {
                    flags |= 16;
                } else if (buffer[*position + i] == 'u') {
                    flags |= 32;
                } else if (buffer[*position + i] == 'x') {
                    flags |= 64;
                }
            }
            *position += flags_length + 1;
            value = PyObject_CallFunction(RECompile, "Oi", pattern, flags);
            Py_DECREF(pattern);
            break;
        }
    case 12:
        {
            int collection_length;
            PyObject* collection;
            PyObject* id;

            *position += 4;
            collection_length = strlen(buffer + *position);
            collection = PyUnicode_DecodeUTF8(buffer + *position, collection_length, "strict");
            if (!collection) {
                return NULL;
            }
            *position += collection_length + 1;
            id = PyObject_CallFunction(ObjectId, "s#", buffer + *position, 12);
            if (!id) {
                Py_DECREF(collection);
                return NULL;
            }
            *position += 12;
            value = PyObject_CallFunctionObjArgs(DBRef, collection, id, NULL);
            Py_DECREF(collection);
            Py_DECREF(id);
            break;
        }
    case 15:
        {
            int code_length,
                scope_size;
            PyObject* code;
            PyObject* scope;

            *position += 8;
            code_length = strlen(buffer + *position);
            code = PyUnicode_DecodeUTF8(buffer + *position, code_length, "strict");
            if (!code) {
                return NULL;
            }
            *position += code_length + 1;

            memcpy(&scope_size, buffer + *position, 4);
            scope = elements_to_dict(buffer + *position + 4, scope_size - 5,
                                     (PyObject*)&PyDict_Type, tz_aware);
            if (!scope) {
                Py_DECREF(code);
                return NULL;
            }
            *position += scope_size;

            value = PyObject_CallFunctionObjArgs(Code, code, scope, NULL);
            Py_DECREF(code);
            Py_DECREF(scope);
            break;
        }
    case 16:
        {
            int i;
            memcpy(&i, buffer + *position, 4);
            value = PyInt_FromLong(i);
            if (!value) {
                return NULL;
            }
            *position += 4;
            break;
        }
    case 17:
        {
            unsigned int time, inc;
            memcpy(&inc, buffer + *position, 4);
            memcpy(&time, buffer + *position + 4, 4);
            value = PyObject_CallFunction(Timestamp, "II", time, inc);
            if (!value) {
                return NULL;
            }
            *position += 8;
            break;
        }
    case 18:
        {
            long long ll;
            memcpy(&ll, buffer + *position, 8);
            value = PyLong_FromLongLong(ll);
            if (!value) {
                return NULL;
            }
            *position += 8;
            break;
        }
    case -1:
        {
            value = PyObject_CallFunctionObjArgs(MinKey, NULL);
            break;
        }
    case 127:
        {
            value = PyObject_CallFunctionObjArgs(MaxKey, NULL);
            break;
        }
    default:
        {
            PyObject* InvalidDocument = _error("InvalidDocument");
            PyErr_SetString(InvalidDocument, "no c decoder for this type yet");
            Py_DECREF(InvalidDocument);
            return NULL;
        }
    }
    return value;
}

static PyObject* elements_to_dict(const char* string, int max,
                                  PyObject* as_class, unsigned char tz_aware) {
    int position = 0;
    PyObject* dict = PyObject_CallObject(as_class, NULL);
    if (!dict) {
        return NULL;
    }
    while (position < max) {
        int type = (int)string[position++];
        int name_length = strlen(string + position);
        PyObject* name = PyUnicode_DecodeUTF8(string + position, name_length, "strict");
        PyObject* value;
        if (!name) {
            return NULL;
        }
        position += name_length + 1;
        value = get_value(string, &position, type, as_class, tz_aware);
        if (!value) {
            return NULL;
        }

        PyObject_SetItem(dict, name, value);
        Py_DECREF(name);
        Py_DECREF(value);
    }
    return dict;
}

static PyObject* _cbson_bson_to_dict(PyObject* self, PyObject* args) {
    unsigned int size;
    Py_ssize_t total_size;
    const char* string;
    PyObject* bson;
    PyObject* as_class;
    unsigned char tz_aware;
    PyObject* dict;
    PyObject* remainder;
    PyObject* result;

    if (!PyArg_ParseTuple(args, "OOb", &bson, &as_class, &tz_aware)) {
        return NULL;
    }

    if (!PyString_Check(bson)) {
        PyErr_SetString(PyExc_TypeError, "argument to _bson_to_dict must be a string");
        return NULL;
    }
    total_size = PyString_Size(bson);
    if (total_size < 5) {
        PyObject* InvalidBSON = _error("InvalidBSON");
        PyErr_SetString(InvalidBSON,
                        "not enough data for a BSON document");
        Py_DECREF(InvalidBSON);
        return NULL;
    }

    string = PyString_AsString(bson);
    if (!string) {
        return NULL;
    }
    memcpy(&size, string, 4);

    if (total_size < size) {
        PyObject* InvalidBSON = _error("InvalidBSON");
        PyErr_SetString(InvalidBSON,
                        "objsize too large");
        Py_DECREF(InvalidBSON);
        return NULL;
    }

    if (string[size - 1]) {
        PyObject* InvalidBSON = _error("InvalidBSON");
        PyErr_SetString(InvalidBSON,
                        "bad eoo");
        Py_DECREF(InvalidBSON);
        return NULL;
    }

    dict = elements_to_dict(string + 4, size - 5, as_class, tz_aware);
    if (!dict) {
        return NULL;
    }
    remainder = PyString_FromStringAndSize(string + size, total_size - size);
    if (!remainder) {
        Py_DECREF(dict);
        return NULL;
    }
    result = Py_BuildValue("OO", dict, remainder);
    Py_DECREF(dict);
    Py_DECREF(remainder);
    return result;
}

static PyObject* _cbson_decode_all(PyObject* self, PyObject* args) {
    unsigned int size;
    Py_ssize_t total_size;
    const char* string;
    PyObject* bson;
    PyObject* dict;
    PyObject* result;
    PyObject* as_class = (PyObject*)&PyDict_Type;
    unsigned char tz_aware = 1;

    if (!PyArg_ParseTuple(args, "O|Ob", &bson, &as_class, &tz_aware)) {
        return NULL;
    }

    if (!PyString_Check(bson)) {
        PyErr_SetString(PyExc_TypeError, "argument to decode_all must be a string");
        return NULL;
    }
    total_size = PyString_Size(bson);
    string = PyString_AsString(bson);
    if (!string) {
        return NULL;
    }

    result = PyList_New(0);

    while (total_size > 0) {
        if (total_size < 5) {
            PyObject* InvalidBSON = _error("InvalidBSON");
            PyErr_SetString(InvalidBSON,
                            "not enough data for a BSON document");
            Py_DECREF(InvalidBSON);
            return NULL;
        }

        memcpy(&size, string, 4);

        if (total_size < size) {
            PyObject* InvalidBSON = _error("InvalidBSON");
            PyErr_SetString(InvalidBSON,
                            "objsize too large");
            Py_DECREF(InvalidBSON);
            return NULL;
        }

        if (string[size - 1]) {
            PyObject* InvalidBSON = _error("InvalidBSON");
            PyErr_SetString(InvalidBSON,
                            "bad eoo");
            Py_DECREF(InvalidBSON);
            return NULL;
        }

        dict = elements_to_dict(string + 4, size - 5, as_class, tz_aware);
        if (!dict) {
            return NULL;
        }
        PyList_Append(result, dict);
        Py_DECREF(dict);
        string += size;
        total_size -= size;
    }

    return result;
}

static PyMethodDef _CBSONMethods[] = {
    {"_dict_to_bson", _cbson_dict_to_bson, METH_VARARGS,
     "convert a dictionary to a string containing it's BSON representation."},
    {"_bson_to_dict", _cbson_bson_to_dict, METH_VARARGS,
     "convert a BSON string to a SON object."},
    {"decode_all", _cbson_decode_all, METH_VARARGS,
     "convert binary data to a sequence of documents."},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC init_cbson(void) {
    PyObject *m;

    PyDateTime_IMPORT;
    m = Py_InitModule("_cbson", _CBSONMethods);
    if (m == NULL) {
        return;
    }

    // TODO we don't do any error checking here, should we be?
    _reload_python_objects();
}