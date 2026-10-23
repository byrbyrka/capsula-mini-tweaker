"""Host-only tests. Python is not installed or used on the speaker."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
event = struct.Struct('@llHHi')
measurements = {}
with tempfile.TemporaryDirectory(prefix='ct-') as folder:
    root = Path(folder)
    for directory in ['run', 'etc/local.d', 'dev/input', 'var/lib/capsule-controls',
                      'sys/bus/platform/devices/display/linedisp.0',
                      'sys/class/leds/clock:colon:top', 'sys/class/leds/clock:colon:bottom']:
        (root / directory).mkdir(parents=True, exist_ok=True)
    (root / 'hardware').write_text('50 0\n')
    panel = root / 'sys/bus/platform/devices/display'
    (panel / 'brightness').write_text('255')
    (panel / 'linedisp.0/message').write_text('    ')
    (panel / 'linedisp.0/map_seg7').write_bytes(bytes(128))
    for name in ['top', 'bottom']:
        for attribute in ['brightness', 'trigger']:
            (root / f'sys/class/leds/clock:colon:{name}/{attribute}').write_text('0')
    ring = root / 'ringled'
    ring_log = root / 'ring.log'
    ring.write_text(f'#!/bin/sh\nprintf "%s\\n" "$*" >> "{ring_log}"\n'
                    'case " $* " in *" --duration "*) sleep 120 & wait;; esac\n')
    ring.chmod(0o755)
    (root / 'etc/capsule-tweaker.conf').write_text(
        f'input=/dev/input/event0\ngrab=0\nringled={ring}\ntimezone=UTC\n')
    fifo = root / 'dev/input/event0'
    os.mkfifo(fifo)
    keeper = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    env = dict(os.environ, CAPSULE_TEST_ROOT=folder)
    daemon = subprocess.Popen([binary, '--daemon'], env=env, stderr=subprocess.PIPE)
    address = str(root / 'run/capsule-controls.sock')

    def wait_for(predicate, timeout=4):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            assert daemon.poll() is None, daemon.stderr.read().decode()
            try:
                if predicate():
                    return
            except (OSError, ValueError):
                pass
            time.sleep(.02)
        raise AssertionError('timeout')

    def request(text, ok=True, raw=False):
        client_address = str(root / 'run/test-client')
        Path(client_address).unlink(missing_ok=True)
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
            client.bind(client_address)
            client.settimeout(3)
            client.connect(address)
            client.send(text.encode())
            response = client.recv(4096).decode()
        Path(client_address).unlink(missing_ok=True)
        assert response.startswith('OK ') == ok, response
        return response if raw else response.split()

    def send_key(code, value):
        os.write(keeper, event.pack(0, 0, 1, code, value))
        time.sleep(.04)

    def tap(code):
        send_key(code, 1)
        send_key(code, 0)
        time.sleep(.05)  # Keep distinct taps outside the 100 ms debounce window.

    def status():
        return list(map(int, request('GET')[1:14]))

    def front_red():
        return any(len(f := line.split()) == 15 and f[2] == 'raw'
                   and [i for i, c in enumerate(f[3:]) if c != '000000'] == [5, 6, 7]
                   for line in ring_log.read_text().splitlines())

    try:
        wait_for(lambda: Path(address).exists())
        assert status()[:3] == [50, 0, 2]
        request('VOL 65')
        assert (root / 'hardware').read_text().strip() == '65 0'
        request('VOL 47', ok=False)
        # Deliberately fragment input_event: the daemon must retain the first bytes.
        data = event.pack(0, 0, 1, 115, 1)
        os.write(keeper, data[:7])
        time.sleep(.04)
        os.write(keeper, data[7:])
        send_key(115, 0)
        wait_for(lambda: status()[0] == 70)
        tap(248)
        assert status()[1] == 1
        wait_for(front_red)
        assert (panel / 'linedisp.0/message').read_bytes() != b' OFF'
        # A chord changes neither the audio state nor the selected screen.
        send_key(248, 1)
        send_key(582, 1)
        send_key(582, 0)
        send_key(248, 0)
        state = status()
        assert state[:3] == [70, 1, 2] and state[5] == 1, state
        tap(115)
        assert status()[3] == 1 and status()[0] == 70
        tap(582)
        assert status()[5] == 0 and status()[1] == 1
        request('SET effect 0')
        request('SET face 3')
        assert status()[4] == 3
        # Direct display frames need no clockdisp/sleep/dd processes.
        wait_for(lambda: (panel / 'linedisp.0/message').read_bytes() == b' \x01\x02 ')
        # Exercise the actual interactive preset editor without requiring root
        # on the development machine (this entry exists only in TESTING builds).
        def preset_menu(data):
            result = subprocess.run([binary, '--test-preset-menu'], input=data,
                                    env=env, text=True, capture_output=True, timeout=8)
            assert result.returncode == 0, result.stderr
            return result.stdout

        output = preset_menu('1\n1\nМой пресет\n3\n37\n6\n12\nff0000\n15\n'
                             '7\n6\n2\n12\n0000ff\n15\n5\n250\n10\n0\n')
        assert 'Пресет сохранён.' in output, output
        red, blue = 'ff0000' * 12, '0000ff' * 12
        record = f'Мой пресет|4|37|3000|250|{red},{blue}'
        assert request('PRESET_GET 0', raw=True) == f'OK {record}'
        assert status()[3] == 4, (request('GET', raw=True), output)
        request('SET effect 5', ok=False)
        request('PRESET_PUT 0 ' + record + 'x' * 2000, ok=False)
        assert request('PRESET_GET 0', raw=True) == f'OK {record}'
        request('EDIT')
        request('PRESET_DEL 0', ok=False)
        tap(115)
        assert status()[3] == 0
        tap(114)
        assert status()[3] == 4
        tap(248)
        assert status()[1] == 1 and status()[3] == 4
        request('MUTE')

        def has_frame(color):
            expected = '-b 37 raw ' + ' '.join([color] * 12)
            return expected in ring_log.read_text().splitlines()

        wait_for(lambda: has_frame('ff0000') and has_frame('0000ff'))
        # Volume feedback interrupts the custom animation; the animation resumes.
        start_line = len(ring_log.read_text().splitlines())
        request('VOL 70')
        wait_for(lambda: any(x.startswith('-b 50 raw ') for x in
                             ring_log.read_text().splitlines()[start_line:]))
        wait_for(lambda: any(x.startswith('-b 37 raw ') for x in
                             ring_log.read_text().splitlines()[start_line:]))
        request('MUTE')
        expected_red = '-b 50 raw ' + ' '.join('690600' if 5 <= i <= 7 else '000000'
                                              for i in range(12))
        wait_for(lambda: ring_log.read_text().splitlines()[-1] == expected_red)
        preset_menu('2\n4\n1\nНе сохранять\n0\n0\n')
        assert request('PRESET_GET 0', raw=True) == f'OK {record}'
        daemon.terminate()
        assert daemon.wait(timeout=5) == 0
        daemon.stderr.close()
        assert 'preset0=' + record in (root / 'etc/capsule-tweaker.conf').read_text()
        daemon = subprocess.Popen([binary, '--daemon'], env=env, stderr=subprocess.PIPE)
        wait_for(lambda: Path(address).exists())
        assert status()[3] == 4 and status()[1] == 1
        assert request('PRESET_GET 0', raw=True) == f'OK {record}'
        output = preset_menu('4\n4\n1\n0\n')
        assert 'Пресет удалён.' in output
        assert status()[3] == 0
        request('PRESET_GET 0', ok=False)
        assert request('PRESETS') == ['OK', '0']
        time.sleep(1.6)
        process_status = Path(f'/proc/{daemon.pid}/status').read_text()
        measurements['host_rss_kib'] = int(next(x.split()[1] for x in process_status.splitlines() if x.startswith('VmRSS:')))
        children = Path(f'/proc/{daemon.pid}/task/{daemon.pid}/children').read_text().split()
        assert children == [], children
        ticks = os.sysconf('SC_CLK_TCK')
        before = Path(f'/proc/{daemon.pid}/stat').read_text().split()
        start = time.monotonic()
        time.sleep(3)
        after = Path(f'/proc/{daemon.pid}/stat').read_text().split()
        cpu = (int(after[13]) + int(after[14]) - int(before[13]) - int(before[14])) / ticks
        measurements['host_cpu_percent_during_face'] = round(100 * cpu / (time.monotonic() - start), 3)
        daemon.terminate()
        assert daemon.wait(timeout=5) == 0
        saved = json.loads((root / 'var/lib/capsule-controls/state.json').read_text())
        assert saved == {'volume': 70, 'muted': True, 'mode': 'face'}, saved
        assert ring_log.read_text().splitlines()[-1] == 'off'
        assert not Path(address).exists()
        menu = subprocess.run([binary], input='0\n', env=env, text=True, capture_output=True)
        # Test binary menu requires root just as the production menu does.
        if os.geteuid() == 0:
            assert menu.returncode == 0 and 'ТВИКЕР НА C' in menu.stdout
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=5)
        os.close(keeper)
        daemon.stderr.close()
print('Native integration: passed', json.dumps(measurements))
Path('build/resource-measurements.json').write_text(json.dumps(measurements, indent=2) + '\n')
