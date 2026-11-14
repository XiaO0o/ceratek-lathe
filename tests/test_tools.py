from types import SimpleNamespace

import pytest

from lathe import cli, ports, records
from lathe.link import GrblLink, GrblTimeout
from lathe.sim import SimulatedGrbl

# -- probe (bench test T2) -------------------------------------------------


def test_probe_against_the_simulator_saves_a_record(tmp_path, capsys):
    code = cli.main(["probe", "--sim", "--out", str(tmp_path)])

    assert code == 0
    (record,) = tmp_path.glob("*_probe.txt")
    text = record.read_text(encoding="utf-8")
    assert text.startswith("# Controller probe (bench test T2)")
    assert "SIMULATOR - not a real board" in text
    for section in ("== boot (after opening the port) ==", "== $I ==", "== $$ ==", "== ? (status report) =="):
        assert section in text
    assert "[VER:1.3a.SIM:]" in text
    assert "$100=80.000" in text
    assert "<Idle|MPos:0.000,0.000,0.000|FS:0,0>" in text
    # commands the firmware does not know are recorded, not fatal
    assert "error:3 (invalid or unsupported '$' command)" in text

    out = capsys.readouterr().out
    assert "The controller answered." in out
    assert "Grbl 1.3a" in out
    assert "16 numbered settings read" in out


def test_record_masks_network_details_unless_raw_is_requested(tmp_path):
    cli.main(["probe", "--sim", "--out", str(tmp_path / "masked")])
    cli.main(["probe", "--sim", "--raw", "--out", str(tmp_path / "raw")])

    masked = next((tmp_path / "masked").glob("*.txt")).read_text(encoding="utf-8")
    raw = next((tmp_path / "raw").glob("*.txt")).read_text(encoding="utf-8")
    assert "SSID=<ssid>:IP=<ip>:MAC=<mac>" in masked
    assert "SIM_NET" not in masked and "192.168.4.1" not in masked
    assert "SSID=SIM_NET:IP=192.168.4.1:MAC=AA-BB-CC-DD-EE-FF" in raw


def test_probe_reports_a_board_that_never_answers(tmp_path):
    board = SimulatedGrbl()
    board.silent = True

    result = cli.run_probe(GrblLink(board), timing=cli.SIMULATOR_TIMING)

    assert not result.responding
    assert result.status is None
    assert any(line.startswith("Grbl ") for line in result.sections[0][1])  # the boot text is still kept


def test_probe_works_when_opening_the_port_does_not_restart_the_board(tmp_path, capsys, monkeypatch):
    # Seen on the real board from macOS: no boot text at all, the controller just answers.
    def already_running():
        board = SimulatedGrbl()
        board.reset_input_buffer()
        return board

    monkeypatch.setattr(cli, "SimulatedGrbl", already_running)

    assert cli.main(["probe", "--sim", "--out", str(tmp_path)]) == 0

    text = next(tmp_path.glob("*.txt")).read_text(encoding="utf-8")
    assert f"== {cli.BOOT_SECTION} ==\n{cli.NO_RESTART}\n" in text
    out = capsys.readouterr().out
    assert "banner   : none (the controller did not restart" in out
    assert "identity : [VER:1.3a.SIM:] [OPT:PHS]" in out


def test_extra_probe_commands_are_sent_after_the_standard_ones():
    board = SimulatedGrbl()

    cli.run_probe(GrblLink(board), cli.PROBE_COMMANDS + ("$X",), cli.SIMULATOR_TIMING)

    assert board.received == ["", *cli.PROBE_COMMANDS, "$X"]


@pytest.mark.parametrize(
    "line, expected",
    [
        ("$Sta/SSID=Home Network 5G", "$Sta/SSID=<redacted>"),
        ("$AP/Password=hunter2:with]odd chars", "$AP/Password=<redacted>"),
        ("[MSG:Connecting Home Network]", "[MSG:Connecting <ssid>]"),
        ("[MSG:Connected with Home Network]", "[MSG:Connected with <ssid>]"),
        ("[MSG:Local access point GRBL_ESP started, 192.168.0.1]", "[MSG:Local access point <ssid> started, <ip>]"),
        ("Current WiFi Mode: AP (24:6F:28:AA:BB:CC)", "Current WiFi Mode: AP (<mac>)"),
        ("SSID: Home", "SSID: <ssid>"),
        # machine data must pass through untouched
        ("<Idle|MPos:10.000,5.000,0.000|FS:0,0|WCO:0.000,0.000,0.000>", None),
        ("$100=400.000", None),
        ("[VER:1.3a.20220421:]", None),
        ("Grbl 3.9 [FluidNC v4.1.1 (wifi) '$' for help]", None),
    ],
)
def test_redaction(line, expected):
    assert records.redact_line(line) == (line if expected is None else expected)


# -- monitor (bench test T3) -----------------------------------------------


def test_monitor_reports_each_press_and_release():
    board = SimulatedGrbl()
    link = GrblLink(board)
    link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)
    script = {2: lambda: board.press("Y"), 4: lambda: board.release("Y"), 5: lambda: board.press("Z")}
    seen = []

    summary = cli.run_monitor(
        link, interval=0, max_polls=8, emit=seen.append, on_poll=lambda n: script.get(n, lambda: None)()
    )

    assert summary.polls == 8
    assert summary.initial == frozenset()
    assert summary.activations == {"Y": 1, "Z": 1}
    assert len(seen) == 4  # start, +Y, -Y, +Z
    assert seen[0].endswith("active inputs: none")
    assert seen[1].endswith("+Y") and seen[2].endswith("-Y") and seen[3].endswith("+Z")


def test_monitor_notes_inputs_that_are_already_active_at_the_start():
    link = GrblLink(SimulatedGrbl(pins="XYZ"))
    link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)

    summary = cli.run_monitor(link, interval=0, max_polls=2, emit=lambda _: None)

    assert summary.initial == frozenset("XYZ")
    assert summary.activations == {}


def test_monitor_stops_when_the_board_goes_quiet():
    board = SimulatedGrbl()
    link = GrblLink(board)
    link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)
    board.silent = True
    link.status = lambda timeout=1.0: GrblLink.status(link, timeout=0.02)  # keep the test fast

    with pytest.raises(GrblTimeout):
        cli.run_monitor(link, interval=0, emit=lambda _: None)


def test_monitor_command_runs_end_to_end_on_the_simulator(capsys):
    assert cli.main(["monitor", "--sim", "--duration", "0.05", "--interval", "0.01"]) == 0

    out = capsys.readouterr().out
    assert "active inputs: none" in out
    assert "status polls." in out


# -- port discovery --------------------------------------------------------


def usb(device, vid, pid, description="USB Serial"):
    return SimpleNamespace(device=device, vid=vid, pid=pid, description=description)


BLUETOOTH = SimpleNamespace(device="/dev/cu.Bluetooth-Incoming-Port", vid=None, pid=None, description="n/a")
CONSOLE = SimpleNamespace(device="/dev/cu.debug-console", vid=None, pid=None, description="n/a")


def test_the_only_ch340_is_chosen_and_builtin_ports_are_ignored():
    chosen = ports.choose(comports=[BLUETOOTH, CONSOLE, usb("/dev/cu.usbserial-110", 0x1A86, 0x7523)])

    assert chosen.device == "/dev/cu.usbserial-110"
    assert chosen.usb_id == "1a86:7523"
    assert "CH340" in chosen.label


def test_an_explicit_port_is_used_without_scanning():
    assert ports.choose("COM7", comports=[]).device == "COM7"


def test_no_adapter_explains_what_to_check():
    with pytest.raises(ports.PortError, match="data-capable"):
        ports.choose(comports=[BLUETOOTH, CONSOLE])


def test_two_adapters_ask_for_an_explicit_choice():
    two = [usb("COM3", 0x1A86, 0x7523), usb("COM9", 0x10C4, 0xEA60)]

    with pytest.raises(ports.PortError, match="--port") as problem:
        ports.choose(comports=two)
    assert "COM3" in str(problem.value) and "COM9" in str(problem.value)


def test_unknown_usb_devices_are_listed_after_known_adapters():
    found = ports.scan([usb("/dev/cu.usbmodem1", 0x2341, 0x0043, "Arduino"), usb("/dev/cu.usbserial-1", 0x1A86, 0x7523)])

    assert [c.device for c in found] == ["/dev/cu.usbserial-1", "/dev/cu.usbmodem1"]
    assert found[1].chip is None
