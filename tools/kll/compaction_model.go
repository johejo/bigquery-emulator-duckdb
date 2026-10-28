package main

import (
	"encoding/json"
	"fmt"
	"io"
	"slices"
)

// This is a count-only hypothesis explorer, not the emulator's KLL implementation.
// Enumerating both parities gives possible outcomes, not their probabilities.
// The bounds keep exact geometric-capacity arithmetic within uint64 and limit work.
func compactionModel(k, steps int, out io.Writer) error {
	if k < 3 || k > 1000000 || steps < 1 || steps > 100000 {
		return fmt.Errorf("model requires 3 <= k <= 1000000 and 1 <= steps <= 100000")
	}
	states := [][]int{{0}}
	for n := 1; n <= steps; n++ {
		next := make(map[string][]int)
		for _, state := range states {
			v := slices.Clone(state)
			v[0]++
			if err := compactCounts(k, v, next); err != nil {
				return fmt.Errorf("n=%d: %w", n, err)
			}
		}
		if len(next) > 10000 {
			return fmt.Errorf("n=%d: model state limit exceeded", n)
		}
		states = states[:0]
		for _, v := range next {
			states = append(states, v)
		}
		slices.SortFunc(states, slices.Compare[[]int])
		if err := json.NewEncoder(out).Encode(struct {
			N      int     `json:"n"`
			States [][]int `json:"possible_level_counts"`
		}{n, states}); err != nil {
			return err
		}
	}
	return nil
}

func compactCounts(k int, counts []int, out map[string][]int) error {
	height := len(counts)
	caps := make([]int, height)
	budget, size := 0, 0
	numerator, denominator := uint64(k), uint64(1)
	for d := 0; d < height; d++ {
		cap := int((numerator + denominator - 1) / denominator)
		if cap < 3 {
			return fmt.Errorf("sampler/cutoff migration is outside this model")
		}
		caps[height-1-d] = cap
		budget += cap
		numerator *= 2
		denominator *= 3
	}
	for _, count := range counts {
		size += count
	}
	if size < budget {
		out[fmt.Sprint(counts)] = counts
		return nil
	}
	for i, count := range counts {
		if count < caps[i] {
			continue
		}
		for parity := 0; parity < 2; parity++ {
			if parity == 1 && count%2 == 0 {
				break // Same retained count for both parities of an even block.
			}
			v := slices.Clone(counts)
			v[i] = 0
			promoted := (count + parity) / 2
			if i == height-1 {
				v = append(v, promoted)
			} else {
				v[i+1] += promoted
			}
			if err := compactCounts(k, v, out); err != nil {
				return err
			}
		}
		return nil // Lowest eligible level is a hypothesis, not a known invariant.
	}
	return fmt.Errorf("model budget reached without an eligible level")
}
