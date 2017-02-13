/*
 * Copyright 2016 Capital One Services, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "convert.h"

#include <Python.h>
#if defined(WIN32) || defined(WIN64)
#include <pstdint.h>
#else
#include <stdint.h>
#endif
#include <stdlib.h>

// Python 2/3 C API and Windows compatibility
#include "_compat.h"

#include "pytypes.h"
#include "util.h"


#define BUFFER_SIZE 8 * 1024


static char buffer[BUFFER_SIZE];


PyObject* byte_to_pylong(unsigned char **data) {
    int8_t b;
    unpack_int8_t(data, &b);
    return PyLong_FromLong((long)b);
}

PyObject* byte_to_pystring(unsigned char **data) {
    int8_t b;
    unpack_int8_t(data, &b);
    return PyUnicode_FromFormat("%d", b);
}

PyObject* char_to_pystring(unsigned char **data, const uint64_t column_length) {
    PyObject *str = PyUnicode_FromStringAndSize((char*)*data, column_length);
    *data += column_length;
    return str;
}

// TODO: add switch for handling different types of common time/timestamp
PyObject* char_to_time(unsigned char **data, const uint64_t column_length) {
    struct tm tm;
    memset(&tm, '\0', sizeof(tm));
    memcpy(buffer, (char*)*data, column_length);
    buffer[column_length] = '\0';
    if (strptime(buffer, "%H:%M:%S", &tm) != NULL) {
        *data += column_length;
        return giraffez_time_from_time(tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
    }
    return char_to_pystring(data, column_length);
}

PyObject* char_to_timestamp(unsigned char **data, const uint64_t column_length) {
    struct tm tm;
    memset(&tm, '\0', sizeof(tm));
    memcpy(buffer, (char*)*data, column_length);
    buffer[column_length] = '\0';
    if (strptime(buffer, "%Y-%m-%d %H:%M:%S", &tm) != NULL) {
        *data += column_length;
        return giraffez_ts_from_datetime(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour,
            tm.tm_min, tm.tm_sec, 0);
    }
    return char_to_pystring(data, column_length);
}

PyObject* date_to_pydate(unsigned char **data) {
    int32_t l, year, month, day;
    unpack_int32_t(data, &l);
    l += 19000000;
    year = l / 10000;
    month = (l % 10000) / 100;
    day = l % 100;
    return giraffez_date_from_datetime(year, month, day, 0, 0, 0, 0);
}

PyObject* date_to_pystring(unsigned char **data) {
    int32_t l, year, month, day;
    char s[11];
    unpack_int32_t(data, &l);
    l += 19000000;
    year = l / 10000;
    month = (l % 10000) / 100;
    day = l % 100;
    sprintf(s, "%04d-%02d-%02d", year, month, day);
    return PyUnicode_FromStringAndSize(s, 10);
}

static PyObject* decimal8_to_pystring(unsigned char **data, const uint16_t column_scale) {
    int8_t b;
    PyObject *s;
    unpack_int8_t(data, &b);
    if (column_scale > 0) {
        const char *fmt = b < 0 ? "-%d.%0*d" : "%d.%0*d";
        int8_t scale = (int8_t)pow(10, column_scale);
        int8_t x = abs(b / scale);
        int8_t y = abs(b % scale);
        s = pystring_from_cformat(fmt, x, column_scale, y);
    } else {
        s = PyUnicode_FromFormat("%d", b);
    }
    return s;
}

static PyObject* decimal16_to_pystring(unsigned char **data, const uint16_t column_scale) {
    int16_t h;
    PyObject *s;
    unpack_int16_t(data, &h);
    if (column_scale > 0) {
        const char *fmt = h < 0 ? "-%d.%0*d" : "%d.%0*d";
        int16_t scale = (int16_t)pow(10, column_scale);
        int16_t x = abs(h / scale);
        int16_t y = abs(h % scale);
        s = pystring_from_cformat(fmt, x, column_scale, y);
    } else {
        s = PyUnicode_FromFormat("%d", h);
    }
    return s;
}

static PyObject* decimal32_to_pystring(unsigned char **data, const uint16_t column_scale) {
    int32_t l;
    PyObject* s;
    unpack_int32_t(data, &l);
    if (column_scale > 0) {
        const char* fmt = l < 0 ? "-%d.%0*d" : "%d.%0*d";
        int32_t scale = (int32_t)pow(10, column_scale);
        int32_t x = labs(l / scale);
        int32_t y = labs(l % scale);
        s = pystring_from_cformat(fmt, x, column_scale, y);
    } else {
        s = PyUnicode_FromFormat("%d", l);
    }
    return s;
}

static PyObject* decimal64_to_pystring(unsigned char **data, const uint16_t column_scale) {
    int64_t q;
    PyObject *s;
    unpack_int64_t(data, &q);
    if (column_scale > 0) {
        const char *fmt = q < 0 ? "-%lld.%0*lld" : "%lld.%0*lld";
        int64_t scale = (int64_t)pow(10, column_scale);
        int64_t x = llabs(q / scale);
        int64_t y = llabs(q % scale);
        s = pystring_from_cformat(fmt, x, column_scale, y);
    } else {
        // PyUnicode_FromFormat does not have a format character for
        // types like long long in Python 2.7, so instead using
        // pystring_from_cformat. Also, %lld should be available on
        // MSVC compilers after Visual Studio 2003.
        s = pystring_from_cformat("%lld", q);
    }
    return s;
}

static PyObject* decimal128_to_pystring(unsigned char **data, const uint16_t column_scale) {
    int64_t q; uint64_t Q;
    PyObject *s, *shift, *upper, *lower, *us, *v;
    PyObject *pw, *sd, *scale, *x, *y, *fmt, *tup;
    unpack_uint64_t(data, &Q);
    unpack_int64_t(data, &q);
    shift = PyLong_FromLong((long)64);
    upper = PyLong_FromLongLong(q);
    lower = PyLong_FromUnsignedLongLong(Q);
    us = PyNumber_Lshift(upper, shift);
    v = PyNumber_Or(us, lower);
    if (column_scale > 0) {
        pw = PyLong_FromLong((long)10);
        sd = PyLong_FromLong((long)column_scale);
        scale = PyNumber_Power(pw, sd, Py_None);
        x = PyNumber_FloorDivide(v, scale);
        y = PyNumber_Remainder(v, scale);
        fmt = PyUnicode_FromFormat("%%d.%%0%dd", column_scale);
        tup = Py_BuildValue("(OO)", x, y);
        s = PyUnicode_Format(fmt, tup);
        Py_DECREF(pw);
        Py_DECREF(sd);
        Py_DECREF(scale);
        Py_DECREF(x);
        Py_DECREF(y);
        Py_DECREF(fmt);
        Py_DECREF(tup);
    } else {
        s = PyObject_Str(v);
    }
    Py_DECREF(v);
    Py_XDECREF(shift);
    Py_XDECREF(upper);
    Py_XDECREF(lower);
    Py_XDECREF(us);
    return s;
}

PyObject* decimal_to_pystring(unsigned char **data, const uint64_t column_length,
        const uint16_t column_scale) {
    switch (column_length) {
        case DECIMAL8:
            return decimal8_to_pystring(data, column_scale);
        case DECIMAL16:
            return decimal16_to_pystring(data, column_scale);
        case DECIMAL32:
            return decimal32_to_pystring(data, column_scale);
        case DECIMAL64:
            return decimal64_to_pystring(data, column_scale);
        case DECIMAL128:
            return decimal128_to_pystring(data, column_scale);
    }
    return NULL;
}

PyObject* decimal_to_pyfloat(unsigned char **data, const uint64_t column_length,
        const uint16_t column_scale) {
    PyObject *obj;
    PyObject *s;
    s = decimal_to_pystring(data, column_length, column_scale);
    obj = PyFloat_FromString(s);
    Py_DECREF(s);
    return obj;
}

PyObject* decimal_to_giraffez_decimal(unsigned char **data, const uint64_t column_length,
        const uint16_t column_scale) {
    return giraffez_decimal_from_pystring(decimal_to_pystring(data,
        column_length, column_scale));
}

PyObject* float_to_pyfloat(unsigned char **data) {
    double d;
    unpack_float(data, &d);
    return PyFloat_FromDouble(d);
}

PyObject* float_to_pystring(unsigned char **data) {
    PyObject *obj;
    PyObject *s;
    obj = float_to_pyfloat(data);
    s = PyObject_Str(obj);
    Py_DECREF(obj);
    return s;
}

PyObject* int_to_pylong(unsigned char **data) {
    int32_t l;
    unpack_int32_t(data, &l);
    return PyLong_FromLong(l);
}

PyObject* int_to_pystring(unsigned char **data) {
    int32_t l;
    unpack_int32_t(data, &l);
    return PyUnicode_FromFormat("%d", l);
}

PyObject* long_to_pylong(unsigned char **data) {
    int64_t q;
    unpack_int64_t(data, &q);
    return PyLong_FromLong((long)q);
}

PyObject* long_to_pystring(unsigned char **data) {
    int64_t q;
    unpack_int64_t(data, &q);
    return PyUnicode_FromFormat("%ld", q);
}

PyObject* pystring_from_cformat(const char *fmt, ...) {
    int length;
    char *ss;
    PyObject *s;
    va_list vargs;
    va_start(vargs, fmt);
    length = vsnprintf(NULL, 0, fmt, vargs);
    va_end(vargs);
    ss = malloc(sizeof(char)*length+1);
    va_start(vargs, fmt);
    vsprintf(ss, fmt, vargs);
    va_end(vargs);
    s = PyUnicode_FromStringAndSize(ss, length);
    free(ss);
    return s;
}

PyObject* short_to_pylong(unsigned char **data) {
    int16_t h;
    unpack_int16_t(data, &h);
    return PyLong_FromLong((long)h);
}

PyObject* short_to_pystring(unsigned char **data) {
    int16_t h;
    unpack_int16_t(data, &h);
    return PyUnicode_FromFormat("%d", h);
}

PyObject* vchar_to_pystring(unsigned char **data) {
    PyObject *str;
    uint16_t H;
    unpack_uint16_t(data, &H);
    str = PyUnicode_FromStringAndSize((char*)*data, H);
    *data += H;
    return str;
}


// PACK

PyObject* pystring_to_vchar(PyObject *s, unsigned char **buf, uint16_t *len) {
    Py_ssize_t length;
    PyObject *temp = NULL;
    char *str;
    // TODO: should check for max length?
    if (_PyUnicode_Check(s)) {
            if ((temp = PyUnicode_AsUTF8String(s)) == NULL) {
            }
            if ((temp = PyUnicode_AsUTF8String(s)) == NULL) {
            }
        }
        Py_DECREF(s);
    } else if (PyBytes_Check(s)) {
        temp = s;
        Py_INCREF(temp);
    } else {
        // TODO:
    }
    // XXX:
    if (temp == NULL) {
        return NULL;
    }
    if ((str = PyBytes_AsString(temp)) == NULL) {
        return NULL;
    }
    length = PyBytes_Size(temp);
    *len += pack_string(buf, str, length);
    Py_DECREF(temp);
    Py_RETURN_NONE;
}

PyObject* pystring_to_char(PyObject *s, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    Py_ssize_t length;
    PyObject *temp = NULL;
    int fill, i;
    char *str;
    // TODO: should check for max length?
    if (_PyUnicode_Check(s)) {
        temp = PyUnicode_AsASCIIString(s);
        Py_DECREF(s);
    } else if (PyBytes_Check(s)) {
        temp = s;
        Py_INCREF(temp);
    }
    length = PyBytes_Size(temp);
    if ((str = PyBytes_AsString(temp)) == NULL) {
        return NULL;
    }
    memcpy(*buf, str, length);
    *buf += length;
    fill = column_length - length;
    for (i = 0; i < fill; i++) {
        *((*buf)++) = (unsigned char)0x20;
    }
    *len += column_length;
    Py_DECREF(temp);
    Py_RETURN_NONE;
}

/*switch (column_length) {*/
    /*case INTEGER8:*/
    /*case INTEGER16:*/
    /*case INTEGER32:*/
    /*case INTEGER64:*/
    /*case INTEGER128:*/
/*}*/

PyObject* pylong_to_byte(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    int8_t b;
    if (!PyLong_Check(item)) {
        return NULL;
    }
    b = PyLong_AsLong(item);
    if (b == (long)-1 && PyErr_Occurred()) {
        return NULL;
    }
    pack_int8_t(buf, b);
    *len += column_length;
    Py_RETURN_NONE;
}

PyObject* pylong_to_short(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    int16_t h;
    if (!PyLong_Check(item)) {
        return NULL;
    }
    h = PyLong_AsLong(item);
    if (h == (long)-1 && PyErr_Occurred()) {
        return NULL;
    }
    pack_int16_t(buf, h);
    *len += column_length;
    Py_RETURN_NONE;
}

PyObject* pylong_to_int(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    int32_t l;
    if (!PyLong_Check(item)) {
        return NULL;
    }
    l = PyLong_AsLong(item);
    if (l == (long)-1 && PyErr_Occurred()) {
        return NULL;
    }
    pack_int32_t(buf, l);
    *len += column_length;
    Py_RETURN_NONE;
}

PyObject* pylong_to_long(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    int64_t q;
    if (!PyLong_Check(item)) {
        return NULL;
    }
    q = PyLong_AsLong(item);
    if (q == (long)-1 && PyErr_Occurred()) {
        return NULL;
    }
    pack_int64_t(buf, q);
    *len += column_length;
    Py_RETURN_NONE;
}

PyObject* pyfloat_to_float(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    double d;
    if (!PyFloat_Check(item)) {
        return NULL;
    }
    d = PyFloat_AsDouble(item);
    if (d == -1.0 && PyErr_Occurred()) {
        return NULL;
    }
    pack_float(buf, d);
    *len += column_length;
    Py_RETURN_NONE;
}
#define dot PyUnicode_FromString(".")
#define hyphen PyUnicode_FromString("-")
#define blank PyUnicode_FromString("")

PyObject* pydate_to_int(PyObject *item, const uint16_t column_length, unsigned char **buf, uint16_t *len) {
    PyObject *s;
    if ((s = PyUnicode_Replace(item, hyphen, blank, -1)) == NULL) {
        return NULL;
    }
    PyObject *d = PyLong_FromUnicodeObject(s, 10);
    int32_t l;
    l = PyLong_AsLong(d);
    l -= 19000000;
    pack_int32_t(buf, l);
    *len += column_length;
    Py_RETURN_NONE;
}

char dbuf[1024];

PyObject* pystring_to_decimal(PyObject *item, const uint16_t column_length, const uint16_t column_scale, unsigned char **buf, uint16_t *len) {
    // CheckUnicode/String
    PyObject *parts;
    if ((parts = PyUnicode_Split(item, dot, 1)) == NULL) {
        return NULL;
    }
    Py_ssize_t l = PyList_Size(parts);
    char *x = "";
    char *y = "";
    PyObject *a;
    PyObject *b;
    if (l == 1) {
        if ((a = PyList_GetItem(parts, 0)) == NULL) {
            return NULL;
        }
        x = PyUnicode_AsUTF8(a);
        y = "";
    } else if (l == 2) {
        if ((a = PyList_GetItem(parts, 0)) == NULL) {
            return NULL;
        }
        if ((b = PyList_GetItem(parts, 1)) == NULL) {
            return NULL;
        }
        x = PyUnicode_AsUTF8(a);
        y = PyUnicode_AsUTF8(b);
    } else {
        printf("WTF DUDE\n");
    }
    int size = sprintf(dbuf, "%s%0*s", x, column_scale, y);
    int64_t q;
    PyObject *s = PyLong_FromString(dbuf, NULL, 10);
    /*if (s == NULL) {*/
        /*printf("It is like super NULL\n");*/
    /*}*/
    q = PyLong_AsLongLong(s);
    pack_int64_t(buf, q);

    /*switch (column_length) {*/
        /*case DECIMAL8:*/
            /*return pystring_to_decimal8(data, column_scale);*/
        /*case DECIMAL16:*/
            /*return pystring_to_decimal16(data, column_scale);*/
        /*case DECIMAL32:*/
            /*return pystring_to_decimal32(data, column_scale);*/
        /*case DECIMAL64:*/
            /*return pystring_to_decimal64(data, column_scale);*/
        /*case DECIMAL128:*/
            /*return pystring_to_decimal128(data, column_scale);*/
    /*}*/
    *len += column_length;
    Py_RETURN_NONE;
}