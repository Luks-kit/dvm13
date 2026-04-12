/* dvm buildfile */

compile(src, obj, cflags) {
    auto cmd, cc;
    cc = "cc";
    if (newer(src, obj)) {
        printf("  CC ");
        printf(src);
        printf("\n");
        cmd = cat(cc, " ", cflags, " -c ", src, " -o ", obj);
        system(cmd);
    }
}

link(objs, out) {
    auto cmd, cc, i;
    cc = "cc";
    if (anynewer(objs, out)) {
        printf("  LD ");
        printf(out);
        printf("\n");
        cmd = cat(cc, " -o ", out);
        i = 0;
        while (strcmp(objs[i], "") != 0) {
            cmd = cat(cmd, " ", objs[i]);
            i = i + 1;
        }
        system(cmd);
    }
}

filter_out(list, name) {
    auto out, i, j;
    out = glob("");  /* scratch */
    i = 0; j = 0;

    while (strcmp(list[i], "") != 0) {
        if (strcmp(list[i], name) != 0) {
            out[j] = list[i];
            j = j + 1;
        }
        i = i + 1;
    }
    out[j] = "";
    return out;
}

collect_sources(dir, list, start) {
    auto files, i, j;
    files = glob(cat(dir, "/*.c"));
    i = 0; j = start;
    while (strcmp(files[i], "") != 0) {
        list[j] = files[i];
        j = j + 1;
        i = i + 1;
    }
    return j;
}

auto CFLAGS     = "-O2 -std=gnu11 -Wall -Wno-unused-function -Wno-unused-but-set-variable -Iinc";
auto CFLAGS_DBG = "-g -fsanitize=address -std=gnu11 -Wall -Wno-unused-function -Wno-unused-but-set-variable -Iinc";
auto CFLAGS_COV = "-g --coverage -std=gnu11 -Wall -Wno-unused-function -Wno-unused-but-set-variable -Iinc";

/* Compile src/*.c → <obj_prefix>*.o, link → out.
   Returns the obj array for test linking. */
build_srcs(flags, obj_prefix, out) {
    auto srcs, objs, i, next;

    srcs = glob("*.c");  /* scratch allocation */
    next = 0;
    next = collect_sources("src", srcs, next);
    srcs[next] = "";

    /* map_prefix/map_postfix work on the whole array */
    objs = map_prefix(srcs, "src/", obj_prefix);
    objs = map_postfix(objs, ".c", ".o");

    system(cat("mkdir -p ", obj_prefix));

    i = 0;
    while (strcmp(srcs[i], "") != 0) {
        compile(srcs[i], objs[i], flags);
        i = i + 1;
    }

    link(objs, out);
    return objs;
}

bin_all() {
    build_srcs(CFLAGS, "bin/objs/", "bin/dvm");
}

bin_debug() {
    build_srcs(CFLAGS_DBG, "bin/dbg/", "bin/dvm_dbg");
}

build_tests(flags, obj_prefix, bin_suffix) {
    auto tsrcs, tobjs, tbins, sobjs, all_objs;
    auto i, j;

    sobjs = build_srcs(flags, "bin/objs/", cat("bin/dvm", bin_suffix));

    sobjs = filter_out(sobjs, "bin/objs/main.o");

    tsrcs = glob("test/*.c");
    all_objs = glob("");  /* scratch allocation, same trick as elsewhere */
    tobjs = map_prefix(tsrcs, "test/", obj_prefix);
    tobjs = map_postfix(tobjs, ".c", ".o");

    tbins = map_prefix(tsrcs, "test/", "bin/");
    tbins = map_postfix(tbins, ".c", bin_suffix);

    system(cat("mkdir -p ", obj_prefix));  

    i = 0;
    while (strcmp(tsrcs[i], "") != 0) {
        compile(tsrcs[i], tobjs[i], flags);

        j = 0;
        while (strcmp(sobjs[j], "") != 0) {
            all_objs[j] = sobjs[j];
            j = j + 1;
        }

        all_objs[j] = tobjs[i];
        j = j + 1;
        all_objs[j] = "";
        
        link(all_objs, tbins[i]);
        i = i + 1;
    }
}

run_tests() {
    auto bins, i;
    build_tests(CFLAGS, "bin/test/", "");
    bins = glob("bin/test_*");
    i = 0;
    while (strcmp(bins[i], "") != 0) {
        printf("  RUN ");
        printf(bins[i]);
        printf("\n");
        system(bins[i]);
        i = i + 1;
    }
}

run_tests_asan() {
    auto bins, i;
    build_tests(CFLAGS_DBG, "bin/asan_", "_asan");
    bins = glob("bin/test_*_asan");
    i = 0;
    while (strcmp(bins[i], "") != 0) {
        printf("  RUN ");
        printf(bins[i]);
        printf("\n");
        system(bins[i]);
        i = i + 1;
    }
}

run_tests_cov() {
    auto bins, i;
    build_tests(CFLAGS_COV, "bin/cov_", "_cov");
    bins = glob("bin/test_*_cov");
    i = 0;
    while (strcmp(bins[i], "") != 0) {
        printf("  RUN ");
        printf(bins[i]);
        printf("\n");
        system(bins[i]);
        i = i + 1;
    }
    system("gcov src/*.c");
}

clean() {
    printf("Cleaning...\n");
    system("find bin/ -type f -delete 2>/dev/null || true");
    system("rm -f *.gcov *.gcda *.gcno");
}

install() {
    bin_all();
    printf("Installing...\n");
    system("cp bin/dvm /usr/local/bin/dvm");
}

compile_commands() {
    printf("Generating compile_commands.json via bear...\n");
    system("bear -- bbuild");
}

main(argc, argv) {
    auto target;
    target = "all";
    if (argc > 1) {
        target = argv[1];
    }

    if (strcmp(target, "all") == 0)     { bin_all();          return 0; }
    if (strcmp(target, "debug") == 0)   { bin_debug();        return 0; }
    if (strcmp(target, "test") == 0)    { run_tests();        return 0; }
    if (strcmp(target, "asan") == 0)    { run_tests_asan();   return 0; }
    if (strcmp(target, "cov") == 0)     { run_tests_cov();    return 0; }
    if (strcmp(target, "clean") == 0)   { clean();            return 0; }
    if (strcmp(target, "install") == 0) { install();          return 0; }
    if (strcmp(target, "compdb") == 0)  { compile_commands(); return 0; }
    if (strcmp(target, "run") == 0)     { bin_all(); system("bin/dvm"); return 0; }

    printf("Unknown target: ");
    printf(target);
    printf("\n");
    printf("Targets: all  debug  test  asan  cov  clean  install  compdb  run\n");
    return 1;
}
