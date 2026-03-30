extern {
    void* malloc(size_t n);
    void free(void* p);
}

struct Buffer<T> {
    T* ptr;
    size_t len;
    size_t capacity;
};

enum Maybe<T> {
    None,
    Some(T),
};

struct Pair {
    int left;
    int right;
};

struct Score {
    int id;
    float weight;
    char tag;
};

struct Trace {
    int value;
};

impl drop<T>(&mut Buffer<T> self) {
    unchecked {
        for (int i = 0; i < self.len; i++) {
            drop(self.ptr[i]);
        }
        free(self.ptr);
    }
}

impl drop(&mut Trace self) {}

struct Stats {
    int number_sum;
    int pair_sum;
    int even_count;
    float weight_total;
    int first_score_id;
};

enum WorkItem {
    Count(int),
    Weight(float),
    Marker(char),
    TraceValue(Trace),
    Empty,
};

Buffer<T> buffer_from_ptr<T>(T* memory, size_t capacity) {
    return {memory, 0, capacity};
}

int buffer_len<T>(&Buffer<T> buffer) {
    return buffer.len;
}

void buffer_push<T>(&mut Buffer<T> buffer, T value) {
    unchecked {
        buffer.ptr[buffer.len] = value;
        buffer.len++;
    }
}

&T buffer_get<T>(&Buffer<T> buffer, size_t index) depends(return on buffer) {
    unchecked {
        return &buffer.ptr[index];
    }
}

&mut T buffer_get_mut<T>(&mut Buffer<T> buffer, size_t index)
    depends(return on buffer) {
    unchecked {
        return &mut buffer.ptr[index];
    }
}

Maybe<T> take_or_none<T>(bool take, T value) {
    if (take) {
        return Some(value);
    }
    return None();
}

Buffer<int> int_buffer_new(size_t capacity) {
    unchecked {
        int* memory = malloc(sizeof(int) * capacity);
        return buffer_from_ptr(memory, capacity);
    }
}

Buffer<Pair> pair_buffer_new(size_t capacity) {
    unchecked {
        Pair* memory = malloc(sizeof(Pair) * capacity);
        return buffer_from_ptr(memory, capacity);
    }
}

Buffer<Score> score_buffer_new(size_t capacity) {
    unchecked {
        Score* memory = malloc(sizeof(Score) * capacity);
        return buffer_from_ptr(memory, capacity);
    }
}

Buffer<Trace> trace_buffer_new(size_t capacity) {
    unchecked {
        Trace* memory = malloc(sizeof(Trace) * capacity);
        return buffer_from_ptr(memory, capacity);
    }
}

Pair make_pair(int base) {
    Pair pair = {base, base + 1};
    return pair;
}

Score make_score(int id, float weight, char tag) {
    Score score = {id, weight, tag};
    return score;
}

Trace make_trace(int value) {
    Trace trace = {value};
    return trace;
}

int pair_sum(Pair pair) { return pair.left + pair.right; }

void fill_numbers(&mut Buffer<int> numbers) {
    for (int i = 0; i < 12; i++) {
        if (i == 5) {
            continue;
        }
        if (i == 8) {
            continue;
        }
        buffer_push(numbers, i);
    }
}

void fill_pairs(&mut Buffer<Pair> pairs) {
    buffer_push(pairs, make_pair(3));
    buffer_push(pairs, make_pair(7));
    buffer_push(pairs, make_pair(10));
}

void seed_scores(&mut Buffer<Score> scores) {
    buffer_push(scores, make_score(1, 1.0, 'a'));
    buffer_push(scores, make_score(2, 2.5, 'x'));
    buffer_push(scores, make_score(3, 0.5, 'z'));
}

void seed_traces(&mut Buffer<Trace> traces) {
    buffer_push(traces, make_trace(2));
    buffer_push(traces, make_trace(5));
    buffer_push(traces, make_trace(6));
}

void shift_pairs(&mut Buffer<Pair> pairs) {
    int len = buffer_len(pairs);
    for (int i = 0; i < len; i++) {
        &mut Pair pair = buffer_get_mut(pairs, i);
        pair.left++;
        if (pair.left % 2 == 0) {
            continue;
        }
        pair.right++;
    }
}

void boost_first_score(&mut Buffer<Score> scores) {
    &mut Score score = buffer_get_mut(scores, 0);
    {
        &mut int id_ref = &mut score.id;
        {
            &int view = id_ref;
            if (*view < 10) {
            }
        }
        *id_ref = *id_ref + 10;
    }
    score.weight = score.weight + 1.25;
}

void normalize_scores(&mut Buffer<Score> scores, float factor) {
    int len = buffer_len(scores);
    for (int i = 0; i < len; i++) {
        &mut Score score = buffer_get_mut(scores, i);
        score.weight = score.weight * factor;
        if (score.id % 2 == 0) {
            continue;
        }
        score.id++;
    }
}

void retag_scores(&mut Buffer<Score> scores) {
    int len = buffer_len(scores);
    for (int i = 0; i < len; i++) {
        &mut Score score = buffer_get_mut(scores, i);
        if (score.id > 10) {
            score.tag = 'b';
            continue;
        }
        if (score.tag == 'x') {
            score.tag = 'y';
        }
    }
}

Maybe<int> first_even(&Buffer<int> numbers) {
    int len = buffer_len(numbers);
    for (int i = 0; i < len; i++) {
        int value = *buffer_get(numbers, i);
        if (value % 2 != 0) {
            continue;
        }
        return Some(value);
    }
    return None();
}

Maybe<int> first_above(&Buffer<int> numbers, int limit) {
    int len = buffer_len(numbers);
    for (int i = 0; i < len; i++) {
        int value = *buffer_get(numbers, i);
        if (value <= limit) {
            continue;
        }
        return Some(value);
    }
    return None();
}

Maybe<Pair> find_large_pair(&Buffer<Pair> pairs, int limit) {
    int len = buffer_len(pairs);
    for (int i = 0; i < len; i++) {
        Pair pair = *buffer_get(pairs, i);
        if (pair.left + pair.right <= limit) {
            continue;
        }
        return Some(pair);
    }
    return None();
}

int sum_with_cursor(&Buffer<int> numbers) {
    unchecked {
        int total = 0;
        int* cursor = numbers.ptr;
        int* end = numbers.ptr + numbers.len;
        while (cursor != end) {
            total = total + *cursor;
            cursor++;
        }
        return total;
    }
}

int sum_even_traces(&Buffer<Trace> traces) {
    int total = 0;
    int len = buffer_len(traces);
    for (int i = 0; i < len; i++) {
        &Trace trace = buffer_get(traces, i);
        if (trace.value % 2 != 0) {
            continue;
        }
        total = total + trace.value;
    }
    return total;
}

Stats summarize(&Buffer<int> numbers, &Buffer<Pair> pairs, &Buffer<Score> scores) {
    Stats stats = {0, 0, 0, 0.0, 0};
    int number_len = buffer_len(numbers);
    for (int i = 0; i < number_len; i++) {
        int value = *buffer_get(numbers, i);
        stats.number_sum = stats.number_sum + value;
        if (value % 2 == 0) {
            stats.even_count++;
        }
    }
    int pair_len = buffer_len(pairs);
    for (int i = 0; i < pair_len; i++) {
        Pair pair = *buffer_get(pairs, i);
        stats.pair_sum = stats.pair_sum + pair.left + pair.right;
    }
    int score_len = buffer_len(scores);
    for (int i = 0; i < score_len; i++) {
        Score score = *buffer_get(scores, i);
        stats.weight_total = stats.weight_total + score.weight;
    }
    &Score first = buffer_get(scores, 0);
    stats.first_score_id = first.id;
    return stats;
}

void adjust_item(&mut WorkItem item) {
    switch (&mut *item) {
        case Count(value):
            *value = *value + 2;
        case Weight(value):
            *value = *value + 0.5;
        case Marker(letter):
            *letter = 'z';
        case TraceValue(trace):
            trace.value = trace.value + 4;
        case Empty:
            return;
    }
}

int score_item(WorkItem item) {
    switch (move item) {
        case Count(value):
            return value;
        case Weight(value):
            if (value > 6.0) {
                return 6;
            }
            return 1;
        case Marker(letter):
            if (letter == 'z') {
                return 26;
            }
            return 1;
        case TraceValue(trace):
            return trace.value;
        case Empty:
            return 0;
    }
}

int main() {
    Buffer<int> numbers = int_buffer_new(24);
    Buffer<Pair> pairs = pair_buffer_new(8);
    Buffer<Score> scores = score_buffer_new(8);
    fill_numbers(&mut numbers);
    fill_pairs(&mut pairs);
    shift_pairs(&mut pairs);
    seed_scores(&mut scores);
    boost_first_score(&mut scores);
    normalize_scores(&mut scores, 2.0);
    retag_scores(&mut scores);

    Maybe<int> maybe_even = first_even(&numbers);
    int even_value = -1;
    switch (&maybe_even) {
        case None:
            return 1;
        case Some(value):
            even_value = *value;
    }

    Maybe<int> maybe_high = first_above(&numbers, 8);
    int high_value = -1;
    switch (move maybe_high) {
        case None:
            return 2;
        case Some(value):
            high_value = value;
    }

    Maybe<Pair> maybe_pair = find_large_pair(&pairs, 20);
    int pair_value = -1;
    switch (move maybe_pair) {
        case None:
            return 3;
        case Some(pair):
            pair_value = pair_sum(pair);
    }

    Maybe<Trace> maybe_trace = take_or_none(true, make_trace(9));
    int trace_value = -1;
    switch (move maybe_trace) {
        case None:
            return 4;
        case Some(trace):
            trace_value = trace.value;
    }

    int item_score = 0;
    {
        WorkItem item = TraceValue(make_trace(5));
        adjust_item(&mut item);
        item_score = score_item(item);
    }

    int trace_block_total = 0;
    {
        Buffer<Trace> traces = trace_buffer_new(4);
        seed_traces(&mut traces);
        trace_block_total = sum_even_traces(&traces);
    }

    int raw_total = sum_with_cursor(&numbers);
    Stats stats = summarize(&numbers, &pairs, &scores);
    &Score first = buffer_get(&scores, 0);
    char first_tag = first.tag;
    if (even_value != 0) {
        return 5;
    }
    if (high_value != 9) {
        return 6;
    }
    if (pair_value != 23) {
        return 7;
    }
    if (trace_value != 9) {
        return 8;
    }
    if (item_score != 9) {
        return 9;
    }
    if (trace_block_total != 8) {
        return 10;
    }
    if (raw_total != 53) {
        return 11;
    }
    if (stats.number_sum != 53) {
        return 12;
    }
    if (stats.pair_sum != 47) {
        return 13;
    }
    if (stats.even_count != 5) {
        return 14;
    }
    if (stats.weight_total != 10.5) {
        return 15;
    }
    if (stats.first_score_id != 12) {
        return 16;
    }
    if (first_tag != 'b') {
        return 17;
    }
    return 0;
}
