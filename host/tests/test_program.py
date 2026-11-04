import pytest

from fakeboard import FakeBoard
from samples import ddr5_4800_16gb
from spdtool.device import Programmer, ProgrammerError
from spdtool.fields import set_field
from spdtool.image import SpdImage
from spdtool.program import apply, prepare


@pytest.fixture(autouse=True)
def tmp_backups(tmp_path, monkeypatch):
    monkeypatch.setattr("spdtool.program.BACKUP_DIR", tmp_path)
    return tmp_path


def _edit(board):
    p = Programmer(transport=board)
    with p.session():
        cur = p.read_image()
        edited = cur.copy()
        set_field(edited, "tRFC1", "410ns")
        target = prepare(cur, edited)
        return p, cur, target


def test_write_with_unlock_relocks_and_verifies(tmp_backups):
    board = FakeBoard(bytes(ddr5_4800_16gb().data), 16, protected=0x01FF)  # blocks 0-8, as shipped
    p, cur, target = _edit(board)
    with p.session():
        apply(p, cur, target, unlock=True, log=lambda s: None)
    assert bytes(board.mem) == bytes(target.data)
    assert board.wp == 0x01FF  # re-locked
    written = SpdImage(bytes(board.mem))
    assert written.crc_ok() and written.serial != cur.serial
    assert len(list(tmp_backups.iterdir())) == 1
    assert board.log[-1] == "power off"


def test_write_refuses_protected_blocks_without_unlock():
    board = FakeBoard(bytes(ddr5_4800_16gb().data), 16, protected=0x0001)
    p, cur, target = _edit(board)
    with p.session(), pytest.raises(ProgrammerError) as e:
        apply(p, cur, target, log=lambda s: None)
    assert e.value.name == "WP"
    assert bytes(board.mem) == bytes(cur.data)
