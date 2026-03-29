enum Result {
    Ok(int),
    Err(int),
};

int unwrap_or_neg_one(Result value) {
    switch (move value) {
        case Ok(payload):
            return payload;
        case Err(payload):
            return -1;
    }
}
