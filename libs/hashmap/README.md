# Hashmap

Small C99 hashmap with string keys and `void *` values.

## Build

```sh
cc -std=c99 -Wall -Wextra -pedantic -O2 your_program.c hashmap.c -o your_program
```

## Use

```c
#include "hashmap.h"

int main(void) {
    hashmap hm;
    void *value;

    if (hm_init(&hm) != HM_OK) return 1;
    hm_put(&hm, "answer", (void *)42, NULL);
    hm_get(&hm, "answer", &value);
    hm_free(&hm);
    return 0;
}
```

## Test

```sh
./test.sh
./fuzz_diff.sh --max-ops 100000
./bench.sh
```

## License

MIT. Copyright (c) 2026 Metailurini.
