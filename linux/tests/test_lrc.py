from core import lrc


def test_parses_centisecond_timestamps():
    parsed = lrc.parse("[00:03.00] first\n[00:04.50] second\n[01:10.25] third\n")
    assert parsed.synced
    assert [l.time for l in parsed.lines] == [3.0, 4.5, 70.25]
    assert parsed.lines[0].text == "first"


def test_millisecond_timestamps_are_not_scaled_as_centiseconds():
    # LRCLIB serves [mm:ss.xxx]. Reading "500" as centiseconds would put
    # this line 5 seconds late.
    parsed = lrc.parse("[00:10.500] half past ten\n[00:11.250] later\n")
    assert [l.time for l in parsed.lines] == [10.5, 11.25]


def test_timestamp_without_fraction():
    parsed = lrc.parse("[00:05] five\n[00:09] nine\n")
    assert [l.time for l in parsed.lines] == [5.0, 9.0]


def test_repeated_timestamps_on_one_line_expand():
    parsed = lrc.parse("[00:10.00][01:30.00] chorus\n[00:20.00] verse\n")
    assert parsed.synced
    # Sorted by time, and the chorus appears at both of its timestamps.
    assert [(l.time, l.text) for l in parsed.lines] == [
        (10.0, "chorus"),
        (20.0, "verse"),
        (90.0, "chorus"),
    ]


def test_id_tags_are_captured_not_rendered():
    parsed = lrc.parse("[ar:Avicii]\n[ti:The Nights]\n[00:03.00] line\n")
    assert parsed.tags["ar"] == "Avicii"
    assert parsed.tags["ti"] == "The Nights"
    assert [l.text for l in parsed.lines] == ["line"]


def test_offset_tag_shifts_times():
    # A positive [offset:] means the lyrics run early, so times move back.
    parsed = lrc.parse("[offset:500]\n[00:10.00] line\n")
    assert parsed.lines[0].time == 9.5


def test_plain_text_is_unsynced():
    parsed = lrc.parse("just some words\nand more words\n")
    assert not parsed.synced
    assert [l.text for l in parsed.lines] == ["just some words", "and more words"]
    assert parsed.index_at(30.0) == -1


def test_mostly_untimestamped_file_is_treated_as_plain_text():
    raw = "[00:01.00] only this one is stamped\n" + "\n".join(f"line {i}" for i in range(10))
    parsed = lrc.parse(raw)
    assert not parsed.synced


def test_index_at_selects_last_elapsed_line():
    parsed = lrc.parse("[00:03.00] a\n[00:06.00] b\n[00:09.00] c\n")
    assert parsed.index_at(0.0) == -1  # before the first line
    assert parsed.index_at(3.0) == 0
    assert parsed.index_at(5.9) == 0
    assert parsed.index_at(6.0) == 1
    assert parsed.index_at(100.0) == 2


def test_empty_input():
    assert not lrc.parse("")
    assert not lrc.parse("   \n  \n")
