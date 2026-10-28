package main

import (
	"bytes"
	"encoding/json"
	"io"
	"reflect"
	"testing"
)

func TestCompactionModel(t *testing.T) {
	// These check the hypothesis explorer's mechanics, not emulator compatibility.
	var out bytes.Buffer
	if err := compactionModel(5, 5, &out); err != nil {
		t.Fatal(err)
	}
	decoder := json.NewDecoder(&out)
	for n := 1; n <= 5; n++ {
		var row struct {
			N      int     `json:"n"`
			States [][]int `json:"possible_level_counts"`
		}
		if err := decoder.Decode(&row); err != nil {
			t.Fatal(err)
		}
		want := [][]int{{n}}
		if n == 5 {
			want = [][]int{{0, 2}, {0, 3}}
		}
		if row.N != n || !reflect.DeepEqual(row.States, want) {
			t.Fatalf("n=%d: %+v", n, row)
		}
	}
}

func TestCompactionModelBounds(t *testing.T) {
	for _, args := range [][2]int{{2, 1}, {1000001, 1}, {5, 0}, {5, 100001}, {3, 3}} {
		if err := compactionModel(args[0], args[1], io.Discard); err == nil {
			t.Fatalf("accepted invalid or sampler-requiring model: %v", args)
		}
	}
}
