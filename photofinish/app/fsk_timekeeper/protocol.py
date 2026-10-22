"""Camera host side of the FSK-WL USB line protocol v8 (device/firmware/src/proto_usb.h).

The Pi owns the master's USB port. It relays the console's lines unchanged and adds its own
`V` commands, one at a time; each is answered by `A V OK` or `X <camera reason>`:

    V S <period>                 start a session, a line every period ticks (0 = stop)
    V C <q> <line> <car> <conf>  a vehicle front first seen on line
    V L <q> <first> <last>       lines first..last may hide a crossing
    V K <q> <line>               every crossing through line has been sent

q counts 1.. per session; resending the last q is answered again without a second record.
C and L consume one capture_seq each (1.. per session), K none.
"""

USB_PROTO = 8
TICKS_PER_S = 16_000_000
PERIOD_MIN = 276
PERIOD_MAX = 16000
CAMERA_REASONS = ("camoff", "camwait", "camfull", "camline", "camseq")


def period_for(line_rate_hz):
    """Trigger period in master ticks for a line rate, within the master's bounds."""
    period = round(TICKS_PER_S / float(line_rate_hz))
    return max(PERIOD_MIN, min(PERIOD_MAX, period))


def line_rate(period):
    return TICKS_PER_S / period


def start(period):
    return f"V S {int(period)}"


def stop():
    return "V S 0"


def crossing(q, line, car, conf):
    return f"V C {int(q)} {int(line)} {int(car)} {int(conf)}"


def loss(q, first, last):
    return f"V L {int(q)} {int(first)} {int(last)}"


def checkpoint(q, line):
    return f"V K {int(q)} {int(line)}"


def camera_node(devid):
    """The camera node of a master: its sender id, the low 8 hex digits of the I line devid."""
    if not isinstance(devid, str) or len(devid) != 16:
        return None
    low = devid[8:].upper()
    return None if low == "00000000" else low


def is_key_command(line):
    """Commands carrying or asking for the fleet key: never relayed over the network."""
    token = line.strip().split(" ", 1)[0]
    return token in ("K", "?KEY")


class Line:
    """One master line: type and tokens."""

    __slots__ = ("text", "tokens")

    def __init__(self, text):
        self.text = text.rstrip("\r\n")
        self.tokens = self.text.split(" ")

    @property
    def kind(self):
        return self.tokens[0] if self.tokens else ""

    def camera_reply(self):
        """'ok' for A V OK, a camera reason for X <reason>, else None."""
        t = self.tokens
        if t[:3] == ["A", "V", "OK"]:
            return "ok"
        if len(t) >= 2 and t[0] == "X" and t[1] in CAMERA_REASONS:
            return t[1]
        return None

    def identity(self):
        """(usb_proto, devid, role) of an I line, else None."""
        t = self.tokens
        if t[0] != "I" or len(t) < 6 or t[1] != "FSK-WL":
            return None
        try:
            return int(t[2]), t[4], t[5]
        except ValueError:
            return None

    def diag_session(self, node):
        """Camera session (sensor_boot_id) and state of node's D line, else None."""
        t = self.tokens
        if t[0] != "D" or len(t) != 33 or t[1] != node:
            return None
        return t[26], t[2]
