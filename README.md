# Introduction
An embedded, GPU accelerated, tiny, fast, on-disk, concurrent key-value store using b+trees.

A database is stored in a single file.

On most POSIX systems, multiple processes can safely access a single file.

Supports put(key,value), get(key), and delete(key).

# Examples
``` C
    db_init(&my_db, argv[1]);
    db_put(&my_db, "hello", "world");
    char* value = db_get(&my_db, "hello");
    printf("%s\n", value);
    db_close(&my_db);
```

# Compile and Execute
`make execute`


