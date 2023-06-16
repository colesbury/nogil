"""A few examples of thread-safety bugs in nogil-3.12 (debug build)."""

import dis
import threading
import types
import sys

# You may need to increase these values (depending on your machine):
WRITERS = 1 << 1
ITEMS = 1 << 10
LOOPS = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000000


class _Crasher:
    # True if the specialization is in the items (not the _read function):
    CHECK_ITEMS = False

    @staticmethod
    def _read(items):
        raise NotImplementedError("_read")

    @staticmethod
    def _write(items):
        raise NotImplementedError("_write")

    @staticmethod
    def _get_items():
        raise NotImplementedError("_get_items")

    @classmethod
    def _check(cls, f):
        for instruction in dis.get_instructions(f, adaptive=True):
            if instruction.opname == cls.__name__:
                return
        raise RuntimeError(f"Missing {cls.__name__} in {f}!")

    @classmethod
    def crash(cls):
        # This might need a few dozen loops in some cases:
        for _ in range(LOOPS):
            print(".", end="", flush=True)
            items = cls._get_items()
            writers = []
            for _ in range(WRITERS):
                writer = threading.Thread(target=cls._write, args=[items])
                writers.append(writer)
            # Specialize:
            for _ in range(2):
                cls._read(items)
            if cls.CHECK_ITEMS:
                for item in items:
                    cls._check(item)
            else:
                cls._check(cls._read)
            # Run:
            for thread in writers:
                thread.start()
            cls._read(items)  # BOOM!
            for thread in writers:
                thread.join()


class BINARY_SUBSCR_GETITEM(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item[None]
            except TypeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.__getitem__
            except AttributeError:
                pass
            type(item).__getitem__ = lambda self, item: None

    @staticmethod
    def _get_items():
        class C:
            __getitem__ = lambda self, item: None

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class BINARY_SUBSCR_LIST_INT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item[0]
            except IndexError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            item.clear()
            item.append(None)

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = [None]
            items.append(item)
        return items


class FOR_ITER_GEN(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                for _ in item:
                    break
            except ValueError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                for _ in item:
                    break
            except ValueError:
                pass

    @staticmethod
    def _get_items():
        def g():
            yield
            yield

        items = []
        for _ in range(ITEMS):
            item = g()
            items.append(item)
        return items


class FOR_ITER_LIST(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            for item in item:
                break

    @staticmethod
    def _write(items):
        for item in items:
            item.clear()
            item.append(None)

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = [None]
            items.append(item)
        return items


class LOAD_ATTR_CLASS(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.a
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.a
            except AttributeError:
                pass
            item.a = object()

    @staticmethod
    def _get_items():
        class C:
            a = object()

        items = []
        for _ in range(ITEMS):
            item = C
            items.append(item)
        return items


class LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.a
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.__getattribute__
            except AttributeError:
                pass
            type(item).__getattribute__ = lambda self, name: None

    @staticmethod
    def _get_items():
        class C:
            __getattribute__ = lambda self, name: None

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class LOAD_ATTR_INSTANCE_VALUE(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            item.a

    @staticmethod
    def _write(items):
        for item in items:
            item.__dict__[None] = None

    @staticmethod
    def _get_items():
        class C:
            pass

        items = []
        for _ in range(ITEMS):
            item = C()
            item.a = None
            items.append(item)
        return items


class LOAD_ATTR_METHOD_LAZY_DICT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.m()
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.m
            except AttributeError:
                pass
            type(item).m = lambda self: None

    @staticmethod
    def _get_items():
        class C(Exception):
            m = lambda self: None

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class LOAD_ATTR_METHOD_NO_DICT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.m()
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.m
            except AttributeError:
                pass
            type(item).m = lambda self: None

    @staticmethod
    def _get_items():
        class C:
            __slots__ = ()
            m = lambda self: None

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class LOAD_ATTR_METHOD_WITH_VALUES(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.m()
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del item.m
            except AttributeError:
                pass
            type(item).m = lambda self: None

    @staticmethod
    def _get_items():
        class C:
            m = lambda self: None

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class LOAD_ATTR_MODULE(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.__name__
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            d = item.__dict__.copy()
            item.__dict__.clear()
            item.__dict__.update(d)

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = types.ModuleType("<item>")
            items.append(item)
        return items


class LOAD_ATTR_PROPERTY(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item.a
            except AttributeError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            try:
                del type(item).a
            except AttributeError:
                pass
            type(item).a = property(lambda self: None)

    @staticmethod
    def _get_items():
        class C:
            a = property(lambda self: None)

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class LOAD_ATTR_WITH_HINT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            item.a

    @staticmethod
    def _write(items):
        for item in items:
            item.__dict__[None] = None

    @staticmethod
    def _get_items():
        class C:
            pass

        items = []
        for _ in range(ITEMS):
            item = C()
            item.__dict__
            item.a = None
            items.append(item)
        return items


class LOAD_GLOBAL_MODULE(_Crasher):
    CHECK_ITEMS = True

    @staticmethod
    def _read(items):
        for item in items:
            item()

    @staticmethod
    def _write(items):
        for item in items:
            item.__globals__[None] = None

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = eval("lambda: x", {"x": None})
            items.append(item)
        return items


class STORE_ATTR_INSTANCE_VALUE(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            item.a = None

    @staticmethod
    def _write(items):
        for item in items:
            item.__dict__[None] = None

    @staticmethod
    def _get_items():
        class C:
            pass

        items = []
        for _ in range(ITEMS):
            item = C()
            items.append(item)
        return items


class STORE_ATTR_WITH_HINT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            item.a = None

    @staticmethod
    def _write(items):
        for item in items:
            item.__dict__[None] = None

    @staticmethod
    def _get_items():
        class C:
            pass

        items = []
        for _ in range(ITEMS):
            item = C()
            item.__dict__
            items.append(item)
        return items


class STORE_SUBSCR_LIST_INT(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                item[0] = None
            except IndexError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            item.clear()
            item.append(None)

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = [None]
            items.append(item)
        return items


class UNPACK_SEQUENCE_LIST(_Crasher):
    @staticmethod
    def _read(items):
        for item in items:
            try:
                [_] = item
            except ValueError:
                pass

    @staticmethod
    def _write(items):
        for item in items:
            item.clear()
            item.append(None)

    @staticmethod
    def _get_items():
        items = []
        for _ in range(ITEMS):
            item = [None]
            items.append(item)
        return items


# TODO: Other specializations that *appear* thread-unsafe:
# - CALL_PY_EXACT_ARGS
# - CALL_PY_WITH_DEFAULTS
# - LOAD_GLOBAL_BUILTIN

# Uncomment one of the following lines to crash the interpreter:

# BINARY_SUBSCR_GETITEM.crash()
# BINARY_SUBSCR_LIST_INT.crash()
# FOR_ITER_GEN.crash()
# FOR_ITER_LIST.crash()
# LOAD_ATTR_CLASS.crash()
# LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN.crash()
# LOAD_ATTR_INSTANCE_VALUE.crash()
LOAD_ATTR_METHOD_LAZY_DICT.crash()
# LOAD_ATTR_METHOD_NO_DICT.crash()
# LOAD_ATTR_METHOD_WITH_VALUES.crash()
# LOAD_ATTR_MODULE.crash()
# LOAD_ATTR_PROPERTY.crash()
# LOAD_ATTR_WITH_HINT.crash()
# LOAD_GLOBAL_MODULE.crash()
# STORE_ATTR_INSTANCE_VALUE.crash()
# STORE_ATTR_WITH_HINT.crash()
# STORE_SUBSCR_LIST_INT.crash()
# UNPACK_SEQUENCE_LIST.crash()