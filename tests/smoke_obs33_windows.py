"""Headless OBS 33 loader test; never reads the user's OBS configuration."""
import argparse
import ctypes as C
import faulthandler
import json
import os
from pathlib import Path
import socket
import sys
import time
import traceback

p = argparse.ArgumentParser()
p.add_argument('--obs-root', required=True, type=Path)
p.add_argument('--plugins-root', required=True, type=Path)
p.add_argument('--config-root', required=True, type=Path)
p.add_argument('--udp-stress', action='store_true')
a = p.parse_args()
faulthandler.enable()
# Fail a blocked native join rather than leave CI waiting indefinitely.
faulthandler.dump_traceback_later(45, exit=True)
a.plugins_root = a.plugins_root.resolve()
a.config_root = a.config_root.resolve()
bin_dir = a.obs_root.resolve() / 'bin' / '64bit'
handle = os.add_dll_directory(str(bin_dir))
os.chdir(bin_dir)
obs = C.CDLL(str(bin_dir / 'obs.dll'))
winsock = C.WinDLL('ws2_32')
wsadata = C.create_string_buffer(512)
assert winsock.WSAStartup(0x0202, C.byref(wsadata)) == 0

class Paths(C.Structure):
    _fields_ = [('binary', C.c_char_p), ('data', C.c_char_p)]
class ModuleInfo(C.Structure):
    _fields_ = [('path_info', Paths), ('type', C.c_int), ('name', C.c_char_p)]
class Failures(C.Structure):
    _fields_ = [('failed_modules', C.POINTER(C.c_char_p)), ('count', C.c_size_t)]

def api(name, result, *args):
    f = getattr(obs, name)
    f.restype, f.argtypes = result, args
    return f

startup = api('obs_startup', C.c_bool, C.c_char_p, C.c_char_p, C.c_void_p)
shutdown = api('obs_shutdown', None)
load = api('obs_load_plugins', None, C.POINTER(ModuleInfo), C.POINTER(Failures))
free_failures = api('obs_module_failure_info_free', None, C.POINTER(Failures))
module_get = api('obs_get_module', C.c_void_p, C.c_char_p)
legacy = api('obs_is_legacy_module', C.c_bool, C.c_void_p)
module_name = api('obs_get_module_name', C.c_char_p, C.c_void_p)
module_binary = api('obs_get_module_binary_path', C.c_char_p, C.c_void_p)
module_data = api('obs_get_module_data_path', C.c_char_p, C.c_void_p)
module_locale = api('obs_module_get_locale_text', C.c_char_p, C.c_void_p, C.c_char_p)
version = api('obs_get_version_string', C.c_char_p)
assert version().decode().startswith('33.'), version()

def exclusive_udp_socket():
    value = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        value.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    except BaseException:
        value.close()
        raise
    return value

def udp_port_is_bound(port):
    # Unlike nonexclusive loopback binds, this detects wildcard listeners on Windows.
    with exclusive_udp_socket() as check:
        try:
            check.bind(('0.0.0.0', port))
        except OSError as error:
            if error.winerror in (10013, 10048):
                return True
            raise
    return False

def wait_udp_ports(ports, *, bound, phase, timeout=3.0):
    deadline = time.monotonic() + timeout
    pending = list(dict.fromkeys(ports))
    while pending:
        pending = [port for port in pending if udp_port_is_bound(port) != bound]
        if not pending:
            return
        if time.monotonic() >= deadline:
            state = 'bound' if bound else 'released'
            raise AssertionError(f'{phase}: expected ports {state}; pending={pending}; UDP workers={udp_worker_threads()}')
        time.sleep(.02)

class ThreadEntry(C.Structure):
    _fields_ = [('size', C.c_ulong), ('usage', C.c_ulong), ('tid', C.c_ulong),
                ('pid', C.c_ulong), ('base_priority', C.c_long),
                ('delta_priority', C.c_long), ('flags', C.c_ulong)]

kernel = C.WinDLL('kernel32', use_last_error=True)
kernel.CreateToolhelp32Snapshot.argtypes = (C.c_ulong, C.c_ulong)
kernel.CreateToolhelp32Snapshot.restype = C.c_void_p
for name in ('Thread32First', 'Thread32Next'):
    function = getattr(kernel, name)
    function.argtypes = (C.c_void_p, C.POINTER(ThreadEntry))
    function.restype = C.c_int
kernel.OpenThread.argtypes = (C.c_ulong, C.c_int, C.c_ulong)
kernel.OpenThread.restype = C.c_void_p
kernel.GetThreadDescription.argtypes = (C.c_void_p, C.POINTER(C.c_void_p))
kernel.GetThreadDescription.restype = C.c_long
kernel.CloseHandle.argtypes = (C.c_void_p,)
kernel.CloseHandle.restype = C.c_int
kernel.LocalFree.argtypes = (C.c_void_p,)
kernel.LocalFree.restype = C.c_void_p

def udp_worker_threads():
    snapshot = kernel.CreateToolhelp32Snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    if snapshot == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    result = []
    entry = ThreadEntry()
    entry.size = C.sizeof(entry)
    try:
        more = kernel.Thread32First(snapshot, C.byref(entry))
        while more:
            if entry.pid == os.getpid():
                thread = kernel.OpenThread(0x0800, False, entry.tid)  # QUERY_LIMITED_INFORMATION
                if thread:
                    description = C.c_void_p()
                    try:
                        if kernel.GetThreadDescription(thread, C.byref(description)) >= 0 and description.value:
                            if C.wstring_at(description.value) == 'move_udp_server_thread':
                                result.append(entry.tid)
                    finally:
                        if description.value:
                            kernel.LocalFree(description)
                        kernel.CloseHandle(thread)
            more = kernel.Thread32Next(snapshot, C.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return result

def wait_udp_worker_count(expected, phase, timeout=3.0):
    deadline = time.monotonic() + timeout
    while True:
        workers = udp_worker_threads()
        if len(workers) == expected:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f'{phase}: expected {expected} UDP workers, got {workers}')
        time.sleep(.02)

udp_test_ports = []
live_sources = set()
live_scenes = set()
attached_filters = {}
a.config_root.mkdir(parents=True, exist_ok=True)
assert startup(b'en-US', str(a.config_root.resolve()).encode(), None), 'obs_startup failed'
try:
    root = a.plugins_root.resolve().as_posix()
    info = ModuleInfo(Paths((root + '/%module%').encode(), (root + '/%module%/data').encode()), 2, None)
    failures = Failures()
    load(C.byref(info), C.byref(failures))
    failed = [failures.failed_modules[i].decode() for i in range(failures.count)]
    free_failures(C.byref(failures))
    assert not failed, failed
    module = module_get(b'move-transition')
    assert module, 'Move module not loaded'
    assert not legacy(module), 'Move was loaded as Legacy'
    expected_root = a.plugins_root / 'move-transition'
    assert Path(module_binary(module).decode()).resolve() == expected_root / 'move-transition.dll'
    assert Path(module_data(module).decode()).resolve() == expected_root / 'data'
    manifest = json.loads((expected_root / 'data' / 'manifest.json').read_text(encoding='utf-8-sig'))
    assert module_name(module).decode() == manifest['display_name']
    assert manifest['version'] == '3.2.2'
    assert manifest['id'] == 'com.github.xSkillZ-ttv.obs-move-transition'
    assert module_locale(module, b'MoveTransition') == b'Move Transition'
    types = set()
    for name in ['obs_enum_filter_types', 'obs_enum_transition_types']:
        enum = api(name, C.c_bool, C.c_size_t, C.POINTER(C.c_char_p))
        i, value = 0, C.c_char_p()
        while enum(i, C.byref(value)):
            types.add(value.value.decode())
            i += 1
    expected = {'move_transition', 'move_transition_override_filter', 'move_source_filter', 'move_source_swap_filter',
                'move_value_filter', 'move_audio_value_filter', 'audio_move_filter',
                'move_action_filter', 'move_audio_action_filter', 'move_directshow_filter'}
    assert expected <= types, f'Missing types: {expected - types}'
    if a.udp_stress:
        data_new = api('obs_data_create', C.c_void_p)
        data_int = api('obs_data_set_int', None, C.c_void_p, C.c_char_p, C.c_longlong)
        data_bool = api('obs_data_set_bool', None, C.c_void_p, C.c_char_p, C.c_bool)
        data_string = api('obs_data_set_string', None, C.c_void_p, C.c_char_p, C.c_char_p)
        data_release = api('obs_data_release', None, C.c_void_p)
        raw_source_new = api('obs_source_create_private', C.c_void_p, C.c_char_p, C.c_char_p, C.c_void_p)
        raw_source_release = api('obs_source_release', None, C.c_void_p)
        source_update = api('obs_source_update', None, C.c_void_p, C.c_void_p)
        source_enabled = api('obs_source_enabled', C.c_bool, C.c_void_p)
        source_set_enabled = api('obs_source_set_enabled', None, C.c_void_p, C.c_bool)
        scene_new = api('obs_scene_create_private', C.c_void_p, C.c_char_p)
        scene_source = api('obs_scene_get_source', C.c_void_p, C.c_void_p)
        scene_release = api('obs_scene_release', None, C.c_void_p)
        filter_add = api('obs_source_filter_add', None, C.c_void_p, C.c_void_p)
        filter_remove = api('obs_source_filter_remove', None, C.c_void_p, C.c_void_p)
        wait_destroy = api('obs_wait_for_destroy_queue', C.c_bool)
        def source_new(*arguments):
            source = raw_source_new(*arguments)
            if source:
                live_sources.add(source)
            return source
        def source_release(source):
            parent = attached_filters.pop(source, None)
            if parent:
                filter_remove(parent, source)
            live_sources.remove(source)
            raw_source_release(source)
        def settings(port):
            value = data_new()
            data_int(value, b'start_trigger', 17)
            data_int(value, b'stop_trigger', 17)
            data_int(value, b'start_trigger_udp_port', port)
            data_int(value, b'stop_trigger_udp_port', port)
            data_string(value, b'start_trigger_udp_packet', b'test-start-only')
            data_string(value, b'stop_trigger_udp_packet', b'test-stop-only')
            data_bool(value, b'custom_duration', True)
            data_int(value, b'duration', 100)
            return value
        def wait_source_enabled(source, expected, phase):
            deadline = time.monotonic() + 3.0
            while source_enabled(source) != expected:
                if time.monotonic() >= deadline:
                    raise AssertionError(f'{phase}: expected source enabled={expected}')
                time.sleep(.01)
        # Reserve distinct free ports, then free them immediately before use.
        reservations = [exclusive_udp_socket() for _ in range(32)]
        for s in reservations:
            s.bind(('0.0.0.0', 0))
        ports = [s.getsockname()[1] for s in reservations]
        udp_test_ports.extend(ports)
        for s in reservations:
            s.close()
        for iteration in range(3):
            sources = []
            for index, port in enumerate(ports * 2):
                value = settings(port)
                source = source_new(b'move_audio_value_filter', f'udp-{iteration}-{index}'.encode(), value)
                data_release(value)
                assert source
                sources.append(source)
            wait_udp_ports(ports, bound=True, phase=f'iteration {iteration}: listener startup')
            wait_udp_worker_count(len(ports), f'iteration {iteration}: one worker per port')
            # Change ports, reuse listeners, then remove filters in uneven order.
            for index, source in enumerate(sources):
                value = settings(ports[(index + 3) % len(ports)])
                source_update(source, value)
                data_release(value)
            for source in sources[::2] + sources[1::2]:
                source_release(source)
            wait_destroy()
            wait_udp_ports(ports, bound=False, phase=f'iteration {iteration}: filter destruction')
            wait_udp_worker_count(0, f'iteration {iteration}: workers terminated')
        # Reproduce bind failure; a second update should be able to retry.
        with exclusive_udp_socket() as occupied:
            occupied.bind(('0.0.0.0', 0))
            busy_port = occupied.getsockname()[1]
            udp_test_ports.append(busy_port)
            value = settings(busy_port)
            source = source_new(b'move_audio_value_filter', b'occupied-port', value)
            assert source
            time.sleep(.15)
        source_update(source, value)
        data_release(value)
        wait_udp_ports([busy_port], bound=True, phase='occupied port: retry creates listener')
        wait_udp_worker_count(1, 'occupied port: retry starts worker')
        source_release(source)
        wait_destroy()
        wait_udp_ports(udp_test_ports, bound=False, phase='all listeners released')
        wait_udp_worker_count(0, 'all filters destroyed')
        # Shutdown immediately after destruction, with no worker grace period.
        value = settings(ports[0])
        source = source_new(b'move_audio_value_filter', b'shutdown-listener', value)
        assert source
        scene = scene_new(b'udp-packet-parent')
        assert scene
        live_scenes.add(scene)
        parent = scene_source(scene)
        filter_add(parent, source)
        attached_filters[source] = parent
        source_set_enabled(source, False)
        data_bool(value, b'enabled_match_moving', True)
        source_update(source, value)
        data_release(value)
        wait_udp_ports([ports[0]], bound=True, phase='shutdown listener startup')
        wait_udp_worker_count(1, 'shutdown listener worker')
        # Exercise real datagrams and callbacks with a valid private parent,
        # without graphics rendering, frontend actions or UI.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.sendto(b'unmatched-packet', ('127.0.0.1', ports[0]))
            time.sleep(.05)
            assert not source_enabled(source), 'unmatched packet triggered the filter'
            sender.sendto(b'test-start-only', ('127.0.0.1', ports[0]))
            wait_source_enabled(source, True, 'UDP start packet')
            sender.sendto(b'test-stop-only', ('127.0.0.1', ports[0]))
            wait_source_enabled(source, False, 'UDP stop packet')
        print('UDP_PACKET_RESULT: unmatched ignored; matching start enables and stop disables', flush=True)
        source_release(source)
        live_scenes.remove(scene)
        scene_release(scene)
        wait_destroy()
        print('UDP_STRESS_RESULT: 194 filters, 32 ports, listeners/workers verified, updates, destruction, exclusive bind failure/retry', flush=True)
    print('SMOKE_RESULT ' + json.dumps({'obs': version().decode(), 'module': module_name(module).decode(),
          'legacy': False, 'binary': module_binary(module).decode(), 'data': module_data(module).decode(),
          'registered_types': sorted(types)}, ensure_ascii=False), flush=True)
finally:
    original_exception = sys.exc_info()
    original_error = original_exception[0]
    if original_error:
        print('SMOKE_FIRST_FAILURE: reporting before cleanup', file=sys.stderr, flush=True)
        traceback.print_exception(*original_exception)
    try:
        if a.udp_stress:
            for source in list(live_sources):
                source_release(source)
            for scene in list(live_scenes):
                live_scenes.remove(scene)
                scene_release(scene)
            if 'wait_destroy' in globals():
                wait_destroy()
        shutdown()
        if a.udp_stress:
            try:
                # Keep Winsock initialized so cleanup cannot hide leaked listeners.
                bound_ports = [port for port in udp_test_ports if udp_port_is_bound(port)]
                workers = udp_worker_threads()
                assert not bound_ports and not workers, f'module shutdown: bound ports={bound_ports}, UDP workers={workers}'
                print('UDP_SHUTDOWN_RESULT: zero UDP workers and all ports released before WSACleanup', flush=True)
            except AssertionError as error:
                print('UDP_SHUTDOWN_FAILURE: ' + str(error), flush=True)
                if not original_error:
                    raise
    finally:
        winsock.WSACleanup()
        faulthandler.cancel_dump_traceback_later()
