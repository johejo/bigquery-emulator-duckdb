package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"strings"
	"testing"
)

func TestInspectRecordedState(t *testing.T) {
	// BigQuery INIT_INT64(1..16, 1), sketch_format.answers.txt.
	const h = "0871101020048a0724080310011a01012201102a0212002a0212002a0212002a0312010432070a011010071803"
	var out bytes.Buffer
	input := "# comment\nSELECT ...\n?> {\"f\":[{\"v\":\"" + h + "\"}]}\n?> error: ignored\n"
	if err := inspect(strings.NewReader(input), "fixture", &out); err != nil {
		t.Fatal(err)
	}
	var got struct {
		Source string `json:"source"`
		Line   int    `json:"line"`
		Cell   string `json:"cell"`
		sketch
	}
	if err := json.Unmarshal(out.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got.Source != "fixture" || got.Line != 3 || got.Cell != "$.f[0].v" {
		t.Fatalf("lost source context: %s", out.Bytes())
	}
	if got.Count != 16 || got.Capacity != 3 || got.Precision != 1 || got.LevelMass != 8 ||
		got.SamplerWeight != 7 || got.SamplerField3 != 3 || !got.HasSampler {
		t.Fatalf("incorrect state: %s", out.Bytes())
	}
	if len(got.Levels) != 4 || len(got.Levels[0].Values) != 0 || len(got.Levels[3].Values) != 1 {
		t.Fatalf("lost empty levels: %s", out.Bytes())
	}
}

func TestMalformedInput(t *testing.T) {
	for _, h := range []string{"", "00", "08", "088080808080808080808080", "0a05", "0a05ff", "09", "0871", "08710871"} {
		t.Run(h, func(t *testing.T) {
			b, err := hex.DecodeString(h)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := decode(b); err == nil {
				t.Fatal("accepted malformed or incomplete input")
			}
		})
	}
}

func TestIntegerSignBoundary(t *testing.T) {
	// Recorded BigQuery level [INT64_MIN, 0, INT64_MAX].
	b, err := hex.DecodeString("8080808080808080800180808080808080808001ffffffffffffffff7f")
	if err != nil {
		t.Fatal(err)
	}
	v, err := values(b, 4, true)
	if err != nil {
		t.Fatal(err)
	}
	want := []int64{-9223372036854775808, 0, 9223372036854775807}
	for i, x := range want {
		if v[i] != x {
			t.Fatalf("value %d: got %v, want %d", i, v[i], x)
		}
	}
}
