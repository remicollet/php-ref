/*
  +----------------------------------------------------------------------+
  | This file is part of the pinepain/php-weak PHP extension.            |
  |                                                                      |
  | Copyright (c) 2016 Bogdan Padalko <zaq178miami@gmail.com>            |
  |                                                                      |
  | Licensed under the MIT license: http://opensource.org/licenses/MIT   |
  |                                                                      |
  | For the full copyright and license information, please view the      |
  | LICENSE file that was distributed with this source or visit          |
  | http://opensource.org/licenses/MIT                                   |
  +----------------------------------------------------------------------+
*/

#include "php_weak_reference.h"
#include "php_weak.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"

zend_class_entry *php_weak_reference_class_entry;
#define this_ce php_weak_reference_class_entry

zend_object_handlers php_weak_reference_object_handlers;

php_weak_reference_t *php_weak_reference_init(zval *this_ptr, zval *referent_zv, zval *notifier_zv);

php_weak_reference_t *php_weak_reference_fetch_object(zend_object *obj) /* {{{ */
{
    return (php_weak_reference_t *) ((char *) obj - XtOffsetOf(php_weak_reference_t, std));
} /* }}} */

php_weak_referent_t *php_weak_referent_find_ptr(zend_ulong h) /* {{{ */
{
    if (NULL == PHP_WEAK_G(referents)) {
        return NULL;
    }

    return (php_weak_referent_t *) zend_hash_index_find_ptr(PHP_WEAK_G(referents), h);
} /* }}} */

#ifdef PHP_WEAK_PATCH_SPL_OBJECT_HASH
static void php_weak_patch_spl_hash() /* {{{ */
{
    if (PHP_WEAK_G(spl_hash_replaced)) {
        return;
    }

    EG(function_table)->pDestructor = NULL;

    zend_function *php_weak_spl_object_hash = zend_hash_str_find_ptr(EG(function_table), "weak\\spl_object_hash", sizeof("Weak\\spl_object_hash")-1);
    zend_function *spl_object_hash          = zend_hash_str_find_ptr(EG(function_table), "spl_object_hash",       sizeof("spl_object_hash")-1);


    if (NULL == php_weak_spl_object_hash) {
        fprintf(stderr, "NULL == php_weak_spl_object_hash\n");
    }

    if (NULL == spl_object_hash) {
        fprintf(stderr, "NULL == spl_object_hash\n");
    }

    zend_hash_str_update_ptr(EG(function_table), "spl_object_hash",       sizeof("spl_object_hash")-1,       php_weak_spl_object_hash);
    zend_hash_str_update_ptr(EG(function_table), "weak\\spl_object_hash", sizeof("weak\\spl_object_hash")-1, spl_object_hash);

    EG(function_table)->pDestructor = ZEND_FUNCTION_DTOR;

    PHP_WEAK_G(spl_hash_replaced) = 1;
} /* }}} */
#endif


void php_weak_reference_call_notifier(zval *reference, zval *notifier) /* {{{ */
{
    zval args;
    zval retval_tmp;

    zend_fcall_info fci;
    zend_fcall_info_cache fci_cache;

    char *errstr;

    if (UNEXPECTED(zend_fcall_info_init(notifier, 0, &fci, &fci_cache, NULL, &errstr))) {
        if (errstr) {
            zend_throw_error(zend_ce_type_error, "Notifier should be a valid callback, %s", errstr);
        } else {
            zend_throw_error(zend_ce_type_error, "Notifier should be a valid callback");
            free(errstr);
        }

        return;
    }

    zend_fcall_info_init(notifier, 0, &fci, &fci_cache, NULL, &errstr);

    if (UNEXPECTED(errstr != NULL)) {
        zend_throw_error(zend_ce_type_error, "Notifier should be to be a valid callback, %s", errstr);
        free(errstr);

        return;
    }

    /* Build the parameter array */
    array_init_size(&args, 1);
    /* First argument to a notifier is weak reference object itself */
    add_index_zval(&args, 0, reference);
    Z_ADDREF_P(reference);

    /* Convert everything to be callable */
    zend_fcall_info_args(&fci, &args);

    fci.retval = &retval_tmp;

    /* Call the function */
    zend_call_function(&fci, &fci_cache);
    fci.retval = NULL;

    /* Clean up our mess */
    zend_fcall_info_args_clear(&fci, 1);

    zval_ptr_dtor(&args);
    zval_ptr_dtor(&retval_tmp);

} /* }}} */

void php_weak_referent_object_dtor_obj(zend_object *object) /* {{{ */
{
    php_weak_referent_t *referent = php_weak_referent_find_ptr(object->handle);

    assert(NULL != referent);
    assert(NULL != PHP_WEAK_G(referents));

    zend_ulong hashIndex;
    zval *hashData;
    zend_string *hashKey;

    php_weak_reference_t *reference;

    referent->original_handlers->dtor_obj(object);

    ZEND_HASH_REVERSE_FOREACH_KEY_VAL(&referent->weak_references, hashIndex, hashKey, hashData)
            {
                reference = (php_weak_reference_t *) Z_PTR_P(hashData);
                reference->referent = NULL;

                switch (Z_TYPE(reference->notifier)) {
                    case IS_NULL:
                        /* no notifier */
                        break;
                    case IS_ARRAY:
                        /* array notifier */
                        add_next_index_zval(&reference->notifier, &reference->this_ptr);
                        Z_ADDREF(reference->this_ptr);
                        break;
                    default:
                        /* callback notifier */
                        if (!EG(exception)) {
                            php_weak_reference_call_notifier(&reference->this_ptr, &reference->notifier);
                        }

                        break;
                }

                zend_hash_index_del(&referent->weak_references, hashIndex);
            }ZEND_HASH_FOREACH_END();

    zend_hash_index_del(PHP_WEAK_G(referents), referent->handle);
} /* }}} */

void php_weak_globals_referents_ht_dtor(zval *zv) /* {{{ */
{
    php_weak_referent_t *referent = (php_weak_referent_t *) Z_PTR_P(zv);

    assert(NULL != referent);

    zend_hash_destroy(&referent->weak_references);
    Z_OBJ(referent->this_ptr)->handlers = referent->original_handlers;

    efree(referent);
} /* }}} */

void php_weak_referent_weak_references_ht_dtor(zval *zv) /* {{{ */
{
    php_weak_reference_t *reference = (php_weak_reference_t *) Z_PTR_P(zv);

    /* clean links to ht & release callbacks as we don't need them already*/
    reference->referent = NULL; /* no need to free anything at this point here */
} /* }}} */

php_weak_referent_t *php_weak_referent_get_or_create(zval *referent_zv) /* {{{ */
{
    php_weak_referent_t *referent = php_weak_referent_find_ptr((zend_ulong) Z_OBJ_HANDLE_P(referent_zv));

    if (referent != NULL) {
        return referent;
    }

#ifdef PHP_WEAK_PATCH_SPL_OBJECT_HASH
    php_weak_patch_spl_hash();
#endif

    referent = (php_weak_referent_t *) ecalloc(1, sizeof(php_weak_referent_t));

    zend_hash_init(&referent->weak_references, 0, NULL, php_weak_referent_weak_references_ht_dtor, 0);
    referent->original_handlers = Z_OBJ_P(referent_zv)->handlers;

    ZVAL_COPY_VALUE(&referent->this_ptr, referent_zv);
    referent->handle = Z_OBJ_HANDLE_P(referent_zv);

    memcpy(&referent->custom_handlers, referent->original_handlers, sizeof(zend_object_handlers));
    referent->custom_handlers.dtor_obj = php_weak_referent_object_dtor_obj;

    Z_OBJ_P(referent_zv)->handlers = &referent->custom_handlers;

    if (NULL == PHP_WEAK_G(referents)) {
        ALLOC_HASHTABLE(PHP_WEAK_G(referents));
        zend_hash_init(PHP_WEAK_G(referents), 1, NULL, php_weak_globals_referents_ht_dtor, 0);
    }

    zend_hash_index_add_ptr(PHP_WEAK_G(referents), (zend_ulong) Z_OBJ_HANDLE_P(referent_zv), referent);

    return referent;
} /* }}} */

void php_weak_reference_attach(php_weak_reference_t *reference, php_weak_referent_t *referent) /* {{{ */
{
    reference->referent = referent;
    zend_hash_index_add_ptr(&referent->weak_references, (zend_ulong) Z_OBJ_HANDLE_P(&reference->this_ptr), reference);
} /* }}} */

void php_weak_reference_unregister(php_weak_reference_t *reference) /* {{{ */
{
    zend_hash_index_del(&reference->referent->weak_references, (zend_ulong) Z_OBJ_HANDLE_P(&reference->this_ptr));
} /* }}} */

void php_weak_reference_maybe_unregister(php_weak_reference_t *reference) /* {{{ */
{
    if (NULL == reference->referent) {
        return;
    }

    php_weak_reference_unregister(reference);
} /* }}} */

php_weak_reference_t *php_weak_reference_init(zval *this_ptr, zval *referent_zv, zval *notifier_zv)  /* {{{ */
{
    php_weak_referent_t *referent;

    PHP_WEAK_REFERENCE_FETCH_INTO(this_ptr, reference);
    ZVAL_COPY_VALUE(&reference->this_ptr, this_ptr);

    referent = php_weak_referent_get_or_create(referent_zv);

    php_weak_reference_attach(reference, referent);

    if (NULL != notifier_zv) {
        ZVAL_COPY(&reference->notifier, notifier_zv);
    } else {
        ZVAL_NULL(&reference->notifier);
    }

    return reference;
} /* }}} */

static void php_weak_reference_check_notifier(zval *notifier, zval *this) /* {{{ */
{
    if (NULL != notifier) {
        switch (Z_TYPE_P(notifier)) {
            case IS_NULL:
                /* no notifier */
                break;
            case IS_ARRAY:
                /* array notifier */
                break;
            default:
                /* maybe callback notifier */
                if (!zend_is_callable(notifier, 0, NULL)) {

                    zend_throw_error(zend_ce_type_error,
                                     "Argument 2 passed to %s::%s() must be callable, array or null, %s given",
                                     ZSTR_VAL(Z_OBJCE_P(this)->name), get_active_function_name(), zend_zval_type_name(notifier));

                    return;
                }
        }
    }
} /* }}} */

static HashTable *php_weak_reference_gc(zval *object, zval **table, int *n) /* {{{ */
{
    PHP_WEAK_REFERENCE_FETCH_INTO(object, reference);

    *table = &reference->notifier;
    *n = 1;

    return zend_std_get_properties(object);
} /* }}} */

static void php_weak_reference_free(zend_object *object) /* {{{ */
{
    php_weak_reference_t *reference = php_weak_reference_fetch_object(object);

    /* unregister weak reference from tracking object, if not done already before at some place (e.g. obj dtored) */
    php_weak_reference_maybe_unregister(reference);

    zval_ptr_dtor(&reference->notifier);
    ZVAL_UNDEF(&reference->notifier);

    /* freeing original object */
    zend_object_std_dtor(&reference->std);
} /* }}} */

static void php_weak_reference_dtor(zend_object *object) /* {{{ */
{
    php_weak_reference_t *reference = php_weak_reference_fetch_object(object);

    /* unregister weak reference from tracking object, if not done already before at some place (e.g. obj dtored) */
    php_weak_reference_maybe_unregister(reference);

    zval_ptr_dtor(&reference->notifier);
    ZVAL_UNDEF(&reference->notifier);

    /* call standard dtor */
    zend_objects_destroy_object(object);
} /* }}} */

static zend_object *php_weak_reference_ctor(zend_class_entry *ce)  /* {{{ */
{
    php_weak_reference_t *reference;

    reference = (php_weak_reference_t *) ecalloc(1, sizeof(php_weak_reference_t) + zend_object_properties_size(ce));

    zend_object_std_init(&reference->std, ce);
    object_properties_init(&reference->std, ce);

    reference->std.handlers = &php_weak_reference_object_handlers;

    return &reference->std;
} /* }}} */

static zend_object *php_weak_reference_clone_obj(zval *object) /* {{{ */
{
    zend_object *old_object;
    zend_object *new_object;

    old_object = Z_OBJ_P(object);

    new_object = php_weak_reference_ctor(old_object->ce);

    php_weak_reference_t *old_reference = php_weak_reference_fetch_object(old_object);
    php_weak_reference_t *new_reference = php_weak_reference_fetch_object(new_object);

    ZVAL_OBJ(&new_reference->this_ptr, new_object);
    ZVAL_COPY(&new_reference->notifier, &old_reference->notifier);

    if (old_reference->referent) {
        php_weak_reference_attach(new_reference, old_reference->referent);
    }

    zend_objects_clone_members(new_object, old_object);

    return new_object;
} /* }}} */

static HashTable *php_weak_get_debug_info(zval *object, int *is_temp) /* {{{ */
{
    HashTable *debug_info;
    zend_string *key;
    HashTable *props;

    PHP_WEAK_REFERENCE_FETCH_INTO(object, reference);
    *is_temp = 1;
    props = Z_OBJPROP_P(object);

    ALLOC_HASHTABLE(debug_info);
    ZEND_INIT_SYMTABLE_EX(debug_info, zend_hash_num_elements(props) + 2, 0);

    zend_hash_copy(debug_info, props, (copy_ctor_func_t) zval_add_ref);

    key = zend_mangle_property_name(ZSTR_VAL(this_ce->name), ZSTR_LEN(this_ce->name), "referent",
                                    sizeof("referent") - 1, 0);

    if (NULL != reference->referent) {
        zend_symtable_update(debug_info, key, &reference->referent->this_ptr);
        Z_TRY_ADDREF(reference->referent->this_ptr);
    } else {
        zval tmp;
        ZVAL_NULL(&tmp);
        zend_symtable_update(debug_info, key, &tmp);
    }

    zend_string_release(key);

    key = zend_mangle_property_name(ZSTR_VAL(this_ce->name), ZSTR_LEN(this_ce->name), "notifier",
                                    sizeof("notifier") - 1, 0);
    zend_symtable_update(debug_info, key, &reference->notifier);
    Z_TRY_ADDREF(reference->notifier);

    zend_string_release(key);

    return debug_info;
}/* }}} */

static int php_weak_compare_objects(zval *object1, zval *object2) /* {{{ */
{
    zval result;
    int res;

    PHP_WEAK_REFERENCE_FETCH_INTO(object1, reference1);
    PHP_WEAK_REFERENCE_FETCH_INTO(object2, reference2);

    /* Compare referent objects */
    if (NULL == reference1->referent && NULL == reference2->referent) {
        /* skip */
    } else if(NULL == reference1->referent) {
        return 1;
    } else if (NULL == reference2->referent) {
        return -1;
    } else {
        res = std_object_handlers.compare_objects(&reference1->referent->this_ptr, &reference2->referent->this_ptr);

        if (res != 0) {
            return res;
        }
    }

    /* Compare notifiers */
    ZVAL_LONG(&result, 0);

    compare_function(&result, &reference1->notifier, &reference2->notifier);

    if (Z_LVAL(result) != 0) {
        return (int) Z_LVAL(result);
    }

    /* Compare standard objects */
    return std_object_handlers.compare_objects(object1, object2);
} /* }}} */

static PHP_METHOD(WeakReference, __construct)  /* {{{ */
{
    zval *referent_zv;
    zval *notifier_zv = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "o|z!", &referent_zv, &notifier_zv) == FAILURE) {
        return;
    }

    php_weak_reference_check_notifier(notifier_zv, getThis());

    php_weak_reference_init(getThis(), referent_zv, notifier_zv);

} /* }}} */

static PHP_METHOD(WeakReference, get)  /* {{{ */
{
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    PHP_WEAK_REFERENCE_FETCH_INTO(getThis(), reference);

    if (NULL == reference->referent) {
        RETURN_NULL();
    }

    RETURN_ZVAL(&reference->referent->this_ptr, 1, 0);
} /* }}} */

static PHP_METHOD(WeakReference, valid)  /* {{{ */
{
    if (zend_parse_parameters_none() == FAILURE) {
        return;
    }

    PHP_WEAK_REFERENCE_FETCH_INTO(getThis(), reference);

    RETURN_BOOL(NULL != reference->referent);
} /* }}} */

static PHP_METHOD(WeakReference, notifier)  /* {{{ */
{
    zval *notifier_zv = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|z!", &notifier_zv) == FAILURE) {
        return;
    }

    PHP_WEAK_REFERENCE_FETCH_INTO(getThis(), reference);

    if (ZEND_NUM_ARGS() < 1) {
        RETURN_ZVAL(&reference->notifier, 1, 0);
    }

    /* Change existent notifier */
    php_weak_reference_check_notifier(notifier_zv, getThis());

    RETVAL_ZVAL(&reference->notifier, 1, 1);

    if (NULL == notifier_zv) {
        ZVAL_NULL(&reference->notifier);
    } else {
        ZVAL_COPY(&reference->notifier, notifier_zv);
    }

} /* }}} */


ZEND_BEGIN_ARG_INFO_EX(arginfo_weak_reference___construct, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
                ZEND_ARG_TYPE_INFO(0, referent, IS_OBJECT, 0)
                ZEND_ARG_INFO(0, notify)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_weak_reference_get, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, IS_OBJECT, NULL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_weak_reference_valid, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, _IS_BOOL, NULL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_weak_reference_notifier, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 0)
                ZEND_ARG_INFO(0, notify)
ZEND_END_ARG_INFO()


static const zend_function_entry php_weak_reference_methods[] = { /* {{{ */
        PHP_ME(WeakReference, __construct, arginfo_weak_reference___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)

        PHP_ME(WeakReference, get, arginfo_weak_reference_get, ZEND_ACC_PUBLIC)
        PHP_ME(WeakReference, valid, arginfo_weak_reference_valid, ZEND_ACC_PUBLIC)
        PHP_ME(WeakReference, notifier, arginfo_weak_reference_notifier, ZEND_ACC_PUBLIC)

        PHP_FE_END
}; /* }}} */


PHP_MINIT_FUNCTION (php_weak_reference) /* {{{ */
{
    zend_class_entry ce;

    INIT_NS_CLASS_ENTRY(ce, "Weak", "Reference", php_weak_reference_methods);
    ce.serialize = zend_class_serialize_deny;
    ce.unserialize = zend_class_unserialize_deny;
    this_ce = zend_register_internal_class(&ce);
    this_ce->create_object = php_weak_reference_ctor;

    memcpy(&php_weak_reference_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));

    php_weak_reference_object_handlers.offset = XtOffsetOf(php_weak_reference_t, std);
    php_weak_reference_object_handlers.free_obj = php_weak_reference_free;
    php_weak_reference_object_handlers.dtor_obj = php_weak_reference_dtor;
    php_weak_reference_object_handlers.get_gc = php_weak_reference_gc;
    php_weak_reference_object_handlers.clone_obj = php_weak_reference_clone_obj;
    php_weak_reference_object_handlers.get_debug_info = php_weak_get_debug_info;
    php_weak_reference_object_handlers.compare_objects = php_weak_compare_objects;

    return SUCCESS;
} /* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */