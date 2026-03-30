struct Pair {
    int x;
    int y;
};

int main() {
    Pair p = {1, 2};
    Pair q = {3, 4};
    int i = 1;
    &int fx = &p.x;
    {
        []&Pair views = [&p, &q];
        &Pair a = views[i];
        fx = &(*a).x;
    }
    q.x = 5;
    return *fx;
}
