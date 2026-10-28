// Command kll inspects recorded BigQuery sketches; it is not a production codec.
// It never queries BigQuery or computes expected query answers.
package main

import (
	"bufio"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"strings"
)

type field struct {
	number uint64
	wire   uint64
	n      uint64
	b      []byte
}

func fields(b []byte) ([]field, error) {
	var result []field
	for len(b) > 0 {
		tag, n := binary.Uvarint(b)
		if n <= 0 || tag>>3 == 0 {
			return nil, fmt.Errorf("invalid tag")
		}
		b = b[n:]
		f := field{number: tag >> 3, wire: tag & 7}
		switch f.wire {
		case 0:
			f.n, n = binary.Uvarint(b)
			if n <= 0 {
				return nil, fmt.Errorf("invalid varint")
			}
			b = b[n:]
		case 2:
			size, n := binary.Uvarint(b)
			if n <= 0 || size > uint64(len(b)-n) {
				return nil, fmt.Errorf("invalid length")
			}
			f.b = b[n : n+int(size)]
			b = b[n+int(size):]
		default:
			return nil, fmt.Errorf("unimplemented wire type %d", f.wire)
		}
		result = append(result, f)
	}
	return result, nil
}

// These helpers deliberately reject ambiguous duplicate singular fields.
func one(fs []field, number, wire uint64) (field, error) {
	var found field
	for _, f := range fs {
		if f.number == number {
			if found.number != 0 || f.wire != wire {
				return field{}, fmt.Errorf("ambiguous field %d", number)
			}
			found = f
		}
	}
	return found, nil
}

func values(b []byte, typ uint64, delta bool) ([]any, error) {
	var result []any
	var sum uint64
	for len(b) > 0 {
		switch typ {
		case 4:
			x, n := binary.Uvarint(b)
			if n <= 0 {
				return nil, fmt.Errorf("invalid INT64")
			}
			b = b[n:]
			if delta {
				sum += x // Unsigned modular accumulation across the INT64 sign boundary.
				x = sum
			}
			result = append(result, int64(x))
		case 10:
			if len(b) < 8 {
				return nil, fmt.Errorf("truncated FLOAT64")
			}
			x := math.Float64frombits(binary.LittleEndian.Uint64(b))
			b = b[8:]
			if math.IsNaN(x) || math.IsInf(x, 0) {
				result = append(result, fmt.Sprint(x)) // JSON has no NaN/infinity.
			} else {
				result = append(result, x)
			}
		default:
			return nil, fmt.Errorf("unimplemented type %d", typ)
		}
	}
	return result, nil
}

type level struct {
	Index  int   `json:"index"`
	Values []any `json:"values"`
}

type sketch struct {
	Count         uint64  `json:"count"`
	Type          uint64  `json:"type"`
	Capacity      uint64  `json:"capacity_field_1"`
	Precision     uint64  `json:"precision"`
	Levels        []level `json:"levels"`
	LevelMass     uint64  `json:"level_mass"`
	SamplerValues []any   `json:"sampler_values"`
	SamplerWeight uint64  `json:"sampler_weight"`
	SamplerField3 uint64  `json:"sampler_field_3"`
	HasSampler    bool    `json:"has_sampler"`
}

func decode(b []byte) (sketch, error) {
	var s sketch
	outer, err := fields(b)
	if err != nil {
		return s, err
	}
	get := func(fs []field, num, wire uint64) field {
		f, e := one(fs, num, wire)
		if e != nil && err == nil {
			err = e
		}
		return f
	}
	if get(outer, 1, 0).n != 113 {
		return s, fmt.Errorf("not a KLL envelope")
	}
	s.Count = get(outer, 2, 0).n
	s.Type = get(outer, 4, 0).n
	payload, e := fields(get(outer, 113, 2).b)
	if e != nil {
		return s, e
	}
	s.Capacity = get(payload, 1, 0).n
	s.Precision = get(payload, 2, 0).n
	for _, f := range payload {
		if f.number != 5 {
			continue
		}
		if f.wire != 2 || len(s.Levels) >= 63 {
			return s, fmt.Errorf("unimplemented level encoding/count")
		}
		fs, e := fields(f.b)
		if e != nil {
			return s, e
		}
		num := uint64(2)
		if s.Type == 10 {
			num = 1
		}
		v, e := values(get(fs, num, 2).b, s.Type, true)
		if e != nil {
			return s, e
		}
		i := len(s.Levels)
		if uint64(len(v)) > (math.MaxUint64-s.LevelMass)>>i {
			return s, fmt.Errorf("level mass overflow")
		}
		s.LevelMass += uint64(len(v)) << i
		s.Levels = append(s.Levels, level{Index: i, Values: v})
	}
	f := get(payload, 6, 2)
	s.HasSampler = f.number != 0
	fs, e := fields(f.b)
	if e != nil {
		return s, e
	}
	s.SamplerValues, e = values(get(fs, 1, 2).b, s.Type, false)
	if e != nil {
		return s, e
	}
	s.SamplerWeight = get(fs, 2, 0).n
	s.SamplerField3 = get(fs, 3, 0).n
	if s.Type != 4 && s.Type != 10 {
		return s, fmt.Errorf("unimplemented type %d", s.Type)
	}
	if s.Capacity == 0 || s.Precision == 0 || len(s.Levels) == 0 {
		return s, fmt.Errorf("missing sketch state")
	}
	return s, err
}

func walk(v any, path string, emit func(string, string) error) error {
	switch v := v.(type) {
	case string:
		// Avoid treating ordinary numeric answers as bytes. The observed KLL
		// envelope begins with algorithm field 1 = 113 (08 71).
		if strings.HasPrefix(strings.ToLower(v), "0871") {
			return emit(path, v)
		}
	case []any:
		for i, x := range v {
			if err := walk(x, fmt.Sprintf("%s[%d]", path, i), emit); err != nil {
				return err
			}
		}
	case map[string]any:
		// bqanswers wraps cells in v/f objects; use a stable traversal order.
		for _, key := range []string{"v", "f"} {
			if x, ok := v[key]; ok {
				if err := walk(x, path+"."+key, emit); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

func inspect(r io.Reader, source string, out io.Writer) error {
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 4096), 16*1024*1024)
	line := 0
	for scanner.Scan() {
		line++
		t := scanner.Text()
		if !strings.HasPrefix(t, "?> ") || strings.HasPrefix(t, "?> error:") {
			continue
		}
		var v any
		if err := json.Unmarshal([]byte(t[3:]), &v); err != nil {
			return fmt.Errorf("%s:%d: %w", source, line, err)
		}
		err := walk(v, "$", func(path, h string) error {
			b, err := hex.DecodeString(h)
			if err != nil {
				return err
			}
			s, err := decode(b)
			if err != nil {
				return err
			}
			return json.NewEncoder(out).Encode(struct {
				Source string `json:"source"`
				Line   int    `json:"line"`
				Cell   string `json:"cell"`
				sketch
			}{source, line, path, s})
		})
		if err != nil {
			return fmt.Errorf("%s:%d: %w", source, line, err)
		}
	}
	return scanner.Err()
}

func main() {
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: go run ./tools/kll ARCHIVE.answers.txt ...")
	}
	flag.Parse()
	if flag.NArg() == 0 {
		flag.Usage()
		os.Exit(2)
	}
	for _, path := range flag.Args() {
		f, err := os.Open(path)
		if err == nil {
			err = inspect(f, path, os.Stdout)
			f.Close()
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
}
