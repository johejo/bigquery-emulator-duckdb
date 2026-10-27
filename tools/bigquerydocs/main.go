// Command bigquerydocs snapshots cited BigQuery documentation and localizes citations.
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const store = "third_party/bigquery_docs"
const origin = "https://docs.cloud.google.com/bigquery/docs"

var remote = regexp.MustCompile(`https?://(?:docs\.)?cloud\.google\.com/bigquery/docs(?:[/#?][^\s<>\x60"'()\[\]{}]*)?`)
var local = regexp.MustCompile(`(?:\.\./)*third_party/bigquery_docs/[A-Za-z0-9_./%-]+\.md(?:#[^\s<>\x60"'()\[\]{}]*)?`)

type page struct {
	URL        string `json:"url"`
	File       string `json:"file"`
	Name       string `json:"name"`
	Title      string `json:"title"`
	SHA256     string `json:"sha256"`
	UpdateTime string `json:"updateTime,omitempty"`
}
type manifest struct {
	Pages []page `json:"pages"`
}
type exception struct {
	File     string `json:"file"`
	Contains string `json:"contains"`
	Reason   string `json:"reason"`
}
type citation struct {
	file, raw, url, fragment string
	line                     int
	replace                  bool
}
type repo struct {
	root          string
	manifest      manifest
	exceptions    []exception
	client        *http.Client
	endpoint, key string
}

func canonical(raw string) (string, string, error) {
	raw = strings.TrimRight(raw, ".,;:")
	u, err := url.Parse(raw)
	if err != nil {
		return "", "", err
	}
	if (u.Scheme != "https" && u.Scheme != "http") || (u.Host != "cloud.google.com" && u.Host != "docs.cloud.google.com") || (u.Path != "/bigquery/docs" && !strings.HasPrefix(u.Path, "/bigquery/docs/")) {
		return "", "", fmt.Errorf("not a BigQuery document: %s", raw)
	}
	q, err := url.ParseQuery(u.RawQuery)
	if err != nil {
		return "", "", err
	}
	for k := range q {
		if k != "hl" {
			return "", "", fmt.Errorf("unknown query parameter %q in %s", k, raw)
		}
	}
	for _, p := range strings.Split(u.Path, "/") {
		if p == ".." || p == "." || strings.Contains(p, "\\") {
			return "", "", fmt.Errorf("unsafe document path: %s", raw)
		}
	}
	fragment := u.EscapedFragment()
	if fragment != "" {
		fragment = "#" + fragment
	}
	return origin + strings.TrimSuffix(strings.TrimPrefix(u.Path, "/bigquery/docs"), "/"), fragment, nil
}
func filename(u string) string {
	p := strings.TrimPrefix(u, origin)
	if p == "" {
		p = "/index"
	}
	return store + p + ".md"
}
func digest(b []byte) string { sum := sha256.Sum256(b); return hex.EncodeToString(sum[:]) }
func (r *repo) read(name string) ([]byte, error) {
	return os.ReadFile(filepath.Join(r.root, filepath.FromSlash(name)))
}
func (r *repo) write(name string, b []byte) error {
	target := filepath.Join(r.root, filepath.FromSlash(name))
	if old, err := os.ReadFile(target); err == nil && bytes.Equal(old, b) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(target), ".bigquerydocs-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	mode := os.FileMode(0644)
	if info, err := os.Stat(target); err == nil {
		mode = info.Mode().Perm()
	}
	if err := f.Chmod(mode); err != nil {
		f.Close()
		return err
	}
	if _, err := f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), target)
}
func (r *repo) load() error {
	b, err := r.read(store + "/manifest.json")
	if err == nil {
		if err := json.Unmarshal(b, &r.manifest); err != nil {
			return err
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	seen := map[string]bool{}
	for _, p := range r.manifest.Pages {
		u, f, err := canonical(p.URL)
		if err != nil || u != p.URL || f != "" || p.File != filename(u) || seen[u] {
			return fmt.Errorf("invalid manifest entry: %s", p.URL)
		}
		seen[u] = true
	}
	b, err = r.read("tools/bigquerydocs/exceptions.json")
	if err != nil {
		return err
	}
	if err := json.Unmarshal(b, &r.exceptions); err != nil {
		return err
	}
	for _, e := range r.exceptions {
		if e.File == "" || e.Contains == "" || e.Reason == "" {
			return errors.New("exception requires file, contains and reason")
		}
	}
	return nil
}
func (r *repo) pages() map[string]page {
	m := map[string]page{}
	for _, p := range r.manifest.Pages {
		m[p.URL] = p
	}
	return m
}
func (r *repo) save() error {
	slices.SortFunc(r.manifest.Pages, func(a, b page) int { return strings.Compare(a.URL, b.URL) })
	b, err := json.MarshalIndent(r.manifest, "", "  ")
	if err != nil {
		return err
	}
	return r.write(store+"/manifest.json", append(b, '\n'))
}
func (r *repo) except(file, line string) bool {
	for _, e := range r.exceptions {
		if file == e.File && strings.Contains(line, e.Contains) {
			return true
		}
	}
	return false
}
func excluded(file string) bool {
	return strings.HasPrefix(file, store+"/") || strings.HasPrefix(file, "tools/bigquerydocs/") || strings.HasPrefix(file, "third_party/")
}
func (r *repo) files() ([]string, error) {
	cmd := exec.Command("git", "ls-files", "-z", "--cached", "--deduplicate")
	cmd.Dir = r.root
	b, err := cmd.Output()
	if err != nil {
		return nil, err
	}
	var files []string
	for _, f := range strings.Split(string(b), "\x00") {
		if f != "" && !excluded(f) {
			files = append(files, f)
		}
	}
	return files, nil
}
func citationLine(file, line string) bool {
	if strings.HasSuffix(file, ".md") {
		return true
	}
	s := strings.TrimSpace(line)
	if strings.HasPrefix(s, "//") || strings.HasPrefix(s, "#") || strings.HasPrefix(s, "--") || strings.HasPrefix(s, "/*") || strings.HasPrefix(s, "*") {
		return true
	}
	// Inline C++/Go comments, but never rewrite a URL inside a string literal.
	var quote byte
	for i := 0; i < len(line); i++ {
		c := line[i]
		if quote != 0 {
			if c == '\\' && quote != '`' {
				i++
				continue
			}
			if c == quote {
				quote = 0
			}
			continue
		}
		if c == '"' || c == '\'' || c == '`' {
			quote = c
			continue
		}
		if i+1 < len(line) && c == '/' && (line[i+1] == '/' || line[i+1] == '*') {
			first := remote.FindStringIndex(line)
			return first == nil || first[0] >= i
		}
	}
	return false
}

// citationLines recognizes comments and scenario descriptions, never API payloads or answers.
func citationLines(file string, lines []string) []bool {
	result := make([]bool, len(lines))
	description := false
	for i, line := range lines {
		if strings.HasSuffix(file, ".yaml") || strings.HasSuffix(file, ".yml") {
			if strings.HasPrefix(line, "description:") {
				description = true
			} else if strings.TrimSpace(line) != "" && len(line) > 0 && line[0] != ' ' && line[0] != '\t' {
				description = false
			}
		}
		result[i] = description || citationLine(file, line)
	}
	return result
}
func (r *repo) scan() ([]citation, []string, error) {
	files, err := r.files()
	if err != nil {
		return nil, nil, err
	}
	var refs []citation
	var problems []string
	for _, file := range files {
		info, err := os.Lstat(filepath.Join(r.root, file))
		if err != nil {
			return nil, nil, err
		}
		if !info.Mode().IsRegular() {
			continue
		}
		b, err := r.read(file)
		if err != nil {
			return nil, nil, err
		}
		if !utf8.Valid(b) || bytes.ContainsRune(b, 0) {
			continue
		}
		lines := strings.Split(string(b), "\n")
		kinds := citationLines(file, lines)
		for i, line := range lines {
			if r.except(file, line) {
				continue
			}
			for _, raw := range remote.FindAllString(line, -1) {
				raw = strings.TrimRight(raw, ".,;:")
				u, f, err := canonical(raw)
				if err != nil {
					problems = append(problems, fmt.Sprintf("%s:%d: %v", file, i+1, err))
					continue
				}
				replace := kinds[i]
				refs = append(refs, citation{file, raw, u, f, i + 1, replace})
				if !replace {
					problems = append(problems, fmt.Sprintf("%s:%d: unclassified URL (add a citation comment or a documented exception): %s", file, i+1, raw))
				}
			}
			for _, raw := range local.FindAllString(line, -1) {
				raw = strings.TrimRight(raw, ".,;:")
				p, f, _ := strings.Cut(raw, "#")
				p = p[strings.Index(p, store+"/"):]
				u := origin + strings.TrimSuffix(strings.TrimPrefix(p, store), ".md")
				if u == origin+"/index" {
					u = origin
				}
				if f != "" {
					f = "#" + f
				}
				refs = append(refs, citation{file, raw, u, f, i + 1, false})
			}
		}
	}
	// The function index uses page#anchor relative to its documented base URL.
	b, err := r.read("tools/bigquery_functions.txt")
	if err == nil {
		for i, line := range strings.Split(string(b), "\n") {
			fields := strings.Fields(line)
			if len(fields) < 2 || strings.HasPrefix(line, "#") {
				continue
			}
			for _, doc := range fields[1:] {
				u, f, err := canonical(origin + "/reference/standard-sql/" + doc)
				if err != nil {
					return nil, nil, err
				}
				refs = append(refs, citation{"tools/bigquery_functions.txt", doc, u, f, i + 1, false})
			}
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, nil, err
	}
	return refs, problems, nil
}
func relative(file, target string) string {
	// Embedded preambles are rendered into docs/, not beside their Go sources.
	if strings.HasPrefix(file, "tools/restprobe/") && strings.HasSuffix(file, "_preamble.md") {
		file = "docs/" + path.Base(file)
	}
	if strings.HasSuffix(file, ".md") {
		p, _ := filepath.Rel(filepath.Dir(filepath.FromSlash(file)), filepath.FromSlash(target))
		return filepath.ToSlash(p)
	}
	return target
}
func (r *repo) verified(p page) error {
	b, err := r.read(p.File)
	if err != nil {
		return err
	}
	if digest(b) != p.SHA256 {
		return fmt.Errorf("snapshot hash mismatch: %s", p.File)
	}
	return nil
}
func (r *repo) rewrite(refs []citation) error {
	pages := r.pages()
	changes := map[string]map[string]string{}
	for _, ref := range refs {
		p, ok := pages[ref.url]
		if !ok || (!ref.replace && !(strings.Contains(ref.raw, store+"/") && strings.Contains(ref.raw, "#"))) {
			continue
		}
		if err := r.verified(p); err != nil {
			return err
		}
		if changes[ref.file] == nil {
			changes[ref.file] = map[string]string{}
		}
		changes[ref.file][ref.raw] = relative(ref.file, p.File)
	}
	files := make([]string, 0, len(changes))
	for f := range changes {
		files = append(files, f)
	}
	slices.Sort(files)
	for _, file := range files {
		b, err := r.read(file)
		if err != nil {
			return err
		}
		lines := strings.Split(string(b), "\n")
		kinds := citationLines(file, lines)
		for i, line := range lines {
			if !kinds[i] || r.except(file, line) {
				continue
			}
			replace := func(raw string) string {
				trimmed := strings.TrimRight(raw, ".,;:")
				if replacement, ok := changes[file][trimmed]; ok {
					return replacement + strings.TrimPrefix(raw, trimmed)
				}
				return raw
			}
			lines[i] = local.ReplaceAllStringFunc(remote.ReplaceAllStringFunc(line, replace), replace)
		}
		if err := r.write(file, []byte(strings.Join(lines, "\n"))); err != nil {
			return err
		}
	}
	return nil
}

type document struct {
	Name       string `json:"name"`
	URI        string `json:"uri"`
	Title      string `json:"title"`
	Content    string `json:"content"`
	UpdateTime string `json:"updateTime"`
}

func (r *repo) fetch(ctx context.Context, u string) (page, error) {
	if err := r.initClient(ctx); err != nil {
		return page{}, err
	}
	name := "documents/" + strings.TrimPrefix(u, "https://")
	endpoint := r.endpoint + "/v1/" + name + "?view=DOCUMENT_VIEW_CONTENT"
	for attempt := 0; attempt < 4; attempt++ {
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
		if err != nil {
			return page{}, err
		}
		if r.key != "" {
			req.Header.Set("X-Goog-Api-Key", r.key)
		}
		resp, err := r.client.Do(req)
		if err != nil {
			return page{}, errors.New("developer knowledge request failed (check network connectivity and authentication)")
		}
		b, readErr := io.ReadAll(io.LimitReader(resp.Body, 32<<20))
		resp.Body.Close()
		if readErr != nil {
			return page{}, readErr
		}
		if resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode >= 500 {
			if attempt < 3 {
				delay := time.Second * time.Duration(1<<attempt)
				retryAfter := resp.Header.Get("Retry-After")
				if seconds, err := strconv.Atoi(retryAfter); err == nil && seconds > 0 && seconds <= 60 {
					if d := time.Duration(seconds) * time.Second; d > delay {
						delay = d
					}
				} else if when, err := http.ParseTime(retryAfter); err == nil && time.Until(when) > delay {
					delay = time.Until(when)
				}
				if delay > time.Minute {
					delay = time.Minute
				}
				timer := time.NewTimer(delay)
				select {
				case <-ctx.Done():
					timer.Stop()
					return page{}, ctx.Err()
				case <-timer.C:
				}
				continue
			}
		}
		if resp.StatusCode != http.StatusOK {
			return page{}, fmt.Errorf("developer knowledge HTTP %d for %s", resp.StatusCode, u)
		}
		var d document
		if err := json.Unmarshal(b, &d); err != nil {
			return page{}, err
		}
		actual, fragment, err := canonical(d.URI)
		if err != nil || actual != u || fragment != "" || d.Name != name || strings.TrimSpace(d.Content) == "" {
			return page{}, fmt.Errorf("unexpected or empty document returned for %s", u)
		}
		p := page{u, filename(u), d.Name, d.Title, digest([]byte(d.Content)), d.UpdateTime}
		if err := r.write(p.File, []byte(d.Content)); err != nil {
			return page{}, err
		}
		return p, nil
	}
	return page{}, errors.New("retry limit exceeded")
}
func (r *repo) run(ctx context.Context, command string, out io.Writer) error {
	refs, problems, err := r.scan()
	if err != nil {
		return err
	}
	wanted := map[string]bool{}
	for _, ref := range refs {
		wanted[ref.url] = true
	}
	pages := r.pages()
	if command == "refresh" {
		for u := range pages {
			wanted[u] = true
		}
	}
	urls := make([]string, 0, len(wanted))
	for u := range wanted {
		urls = append(urls, u)
	}
	slices.Sort(urls)
	for _, u := range urls {
		p, exists := pages[u]
		if command == "scan" {
			status := "missing"
			if exists {
				status = "stored"
			}
			fmt.Fprintf(out, "%s %s\n", status, u)
			seen := map[string]bool{}
			for _, ref := range refs {
				if ref.url == u && !seen[ref.file] {
					fmt.Fprintf(out, "  %s\n", ref.file)
					seen[ref.file] = true
				}
			}
			continue
		}
		if command == "check" {
			if !exists {
				problems = append(problems, "missing snapshot: "+u)
			}
			continue
		}
		if exists && command != "refresh" {
			if err := r.verified(p); err != nil {
				problems = append(problems, err.Error())
			}
			continue
		}
		if err := r.initClient(ctx); err != nil {
			return err
		}
		fmt.Fprintf(out, "fetch %s\n", u)
		p, err = r.fetch(ctx, u)
		if err != nil {
			problems = append(problems, err.Error())
			continue
		}
		pages[u] = p
		r.manifest.Pages = nil
		for _, saved := range pages {
			r.manifest.Pages = append(r.manifest.Pages, saved)
		}
		if err := r.save(); err != nil {
			return err
		}
	}
	if command == "check" {
		for _, p := range r.manifest.Pages {
			if err := r.verified(p); err != nil {
				problems = append(problems, err.Error())
			}
		}
		for _, ref := range refs {
			if ref.replace {
				problems = append(problems, fmt.Sprintf("%s:%d: citation is not localized: %s", ref.file, ref.line, ref.raw))
			}
			if strings.Contains(ref.raw, store+"/") {
				p, ok := pages[ref.url]
				if !ok {
					continue
				}
				if strings.Contains(ref.raw, "#") {
					problems = append(problems, fmt.Sprintf("%s:%d: local citation must link to the file without an anchor: %s", ref.file, ref.line, ref.raw))
				}
				if ref.raw != relative(ref.file, p.File)+ref.fragment {
					problems = append(problems, fmt.Sprintf("%s:%d: incorrect local path: %s", ref.file, ref.line, ref.raw))
				}
			}
		}
	}
	for _, p := range r.manifest.Pages {
		if !wanted[p.URL] {
			fmt.Fprintf(out, "unreferenced %s\n", p.File)
		}
	}
	if command == "sync" || command == "refresh" {
		if err := r.rewrite(refs); err != nil {
			return err
		}
	}
	if len(problems) > 0 {
		slices.Sort(problems)
		problems = slices.Compact(problems)
		return errors.New(strings.Join(problems, "\n"))
	}
	return nil
}

// render localizes generated reports using only verified, already stored pages.
func (r *repo) render(file string, b []byte) ([]byte, error) {
	pages := r.pages()
	for _, p := range pages {
		if err := r.verified(p); err != nil {
			return nil, err
		}
	}
	b = remote.ReplaceAllFunc(b, func(raw []byte) []byte {
		trimmed := strings.TrimRight(string(raw), ".,;:")
		u, _, err := canonical(trimmed)
		if err != nil {
			return raw
		}
		p, ok := pages[u]
		if !ok {
			return raw
		}
		return []byte(relative(file, p.File) + strings.TrimPrefix(string(raw), trimmed))
	})
	return local.ReplaceAllFunc(b, func(raw []byte) []byte {
		trimmed := strings.TrimRight(string(raw), ".,;:")
		target, _, _ := strings.Cut(trimmed, "#")
		snapshot := target[strings.Index(target, store+"/"):]
		for _, p := range pages {
			if snapshot == p.File {
				return []byte(target + strings.TrimPrefix(string(raw), trimmed))
			}
		}
		return raw
	}), nil
}
func main() {
	if err := mainErr(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func mainErr() error {
	if len(os.Args) < 2 {
		return errors.New("usage: bigquerydocs scan|sync|refresh|check|render [output-file]")
	}
	cmd := exec.Command("git", "rev-parse", "--show-toplevel")
	b, err := cmd.Output()
	if err != nil {
		return err
	}
	r := &repo{root: strings.TrimSpace(string(b)), endpoint: "https://developerknowledge.googleapis.com", key: os.Getenv("DEVELOPERKNOWLEDGE_API_KEY")}
	if err := r.load(); err != nil {
		return err
	}
	command := os.Args[1]
	if command == "render" {
		if len(os.Args) != 3 {
			return errors.New("usage: bigquerydocs render output-file")
		}
		b, err := io.ReadAll(os.Stdin)
		if err != nil {
			return err
		}
		b, err = r.render(os.Args[2], b)
		if err != nil {
			return err
		}
		_, err = os.Stdout.Write(b)
		return err
	}
	if len(os.Args) != 2 || !slices.Contains([]string{"scan", "sync", "refresh", "check"}, command) {
		return errors.New("usage: bigquerydocs scan|sync|refresh|check")
	}
	return r.run(context.Background(), command, os.Stdout)
}
