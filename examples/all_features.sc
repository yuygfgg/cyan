import http_parse.println;

export enum Option<T> {
    None,
    Some(T)
};

export struct BufferView {
    []int data;
    bool active;
};

export interface<T> int compute(&T value);

impl compute(&int value) {
    return *value;
}

impl drop(&mut BufferView self) {
    self.active = false;
}

export BufferView make_view([]int arr, int skip) depends(return.data on arr) {
    if (skip >= len(arr)) {
        return { subslice(arr, 0, 0), false }; 
    }
    return { subslice(arr, skip, len(arr) - skip), true };
}

export Option<&T> first_element<T>([]T slice) depends(return.Some on slice) {
    if (len(slice) > 0) {
        return Some(&slice[0]);
    }
    return None();
}

export int main() {
    int[5] numbers = [10, 20, 30, 40, 50];
    []int slice = subslice(numbers, 0, len(numbers));

    BufferView view = make_view(slice, 2);

    Option<&int> first = first_element(view.data);
    int result = 0;

    switch (&first) {
        case Some(val):
            result = **val;
        case None:
            result = -1;
    }

    &compute comp_ref = &result; 
    int computed = compute(comp_ref);

    unchecked {
        int* raw_ptr = &mut result;
        *raw_ptr = (computed as int) + 5;
    }

    []&fmt args = [&result];
    println("Result: {}", args);

    return 0;
}