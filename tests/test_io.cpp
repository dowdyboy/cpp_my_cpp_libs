// =============================================================================
//  io 组件：路径运算、读写、原子替换、目录操作、临时文件
//
//  注意：ctest 的工作目录是 build/，用例并行跑（-j），所以每个用例的临时
//  目录名必须唯一（带用例名），并在结束时删干净。
// =============================================================================
#include "my_cpp_libs/io/io.hpp"

#include <string>

#include "test_framework.hpp"

MYTEST_SUITE("io");

namespace {

std::string case_path(const char* name) { return std::string("mylib_io_test_") + name; }

/// 每个用例结束都清一次（用例中途失败也不会留垃圾给下一个用例）
struct Cleanup {
    std::string path;
    ~Cleanup() { (void)my::io::remove_all(path); }
};

}  // namespace

MYTEST_CASE(io_path_helpers) {
    CHECK_EQ(my::io::join("a", "b"), std::string("a/b"));
    CHECK_EQ(my::io::join("a/", "b"), std::string("a/b"));
    CHECK_EQ(my::io::join("a", "/b"), std::string("a/b"));   // 不产生 "a//b"
    CHECK_EQ(my::io::join("", "b"), std::string("b"));
    CHECK_EQ(my::io::join("a", ""), std::string("a"));

    CHECK_EQ(my::io::filename("dir/sub/file.json"), std::string("file.json"));
    CHECK_EQ(my::io::stem("dir/sub/file.json"), std::string("file"));
    CHECK_EQ(my::io::extension("dir/sub/file.json"), std::string(".json"));
    CHECK_EQ(my::io::extension("noext"), std::string(""));
    CHECK_EQ(my::io::parent("dir/sub/file.json"), std::string("dir/sub"));

    // 当前目录非空，且 absolute 之后仍然存在
    const std::string cwd = my::io::current_dir();
    CHECK(!cwd.empty());
    CHECK(my::io::is_directory(cwd));
}

MYTEST_CASE(io_write_read_roundtrip) {
    const std::string path = case_path("roundtrip.txt");
    Cleanup           cleanup{path};

    CHECK_OK(my::io::write_file(path, "hello 世界"));
    CHECK(my::io::exists(path));

    auto read = my::io::read_file(path);
    CHECK_RESULT(read);
    CHECK_EQ(read.value(), std::string("hello 世界"));

    auto sz = my::io::file_size(path);
    CHECK_RESULT(sz);
    CHECK_EQ(sz.value(), static_cast<std::uintmax_t>(std::string("hello 世界").size()));
}

MYTEST_CASE(io_write_replaces_atomically) {
    const std::string path = case_path("atomic.txt");
    Cleanup           cleanup{path};

    CHECK_OK(my::io::write_file(path, "first version"));
    CHECK_OK(my::io::write_file(path, "second"));

    auto read = my::io::read_file(path);
    CHECK_RESULT(read);
    CHECK_EQ(read.value(), std::string("second"));  // 短内容不能残留旧尾巴

    // 原子替换不该留下临时文件：目录里应该只有目标文件
    const std::string dir = my::io::parent(path).empty() ? std::string(".") : my::io::parent(path);
    auto              names = my::io::list_dir(dir);
    CHECK_RESULT(names);
    std::size_t leftovers = 0;
    for (const auto& n : names.value()) {
        if (n.find(".tmp") != std::string::npos) ++leftovers;
    }
    CHECK_EQ(leftovers, std::size_t{0});
}

MYTEST_CASE(io_read_missing_is_not_found) {
    auto read = my::io::read_file(case_path("does_not_exist.txt"));
    CHECK(!read);
    CHECK_EQ(read.status().code(), my::Errc::NotFound);
}

MYTEST_CASE(io_write_to_missing_directory_fails) {
    // 父目录不存在时不能静默创建：调用方应该显式 create_directories
    auto st = my::io::write_file("mylib_io_no_such_dir/x.txt", "data");
    CHECK(!st);
    CHECK(st.code() == my::Errc::IoError || st.code() == my::Errc::NotFound);
}

MYTEST_CASE(io_append_accumulates) {
    const std::string path = case_path("append.txt");
    Cleanup           cleanup{path};

    CHECK_OK(my::io::write_file(path, "a"));
    CHECK_OK(my::io::append_file(path, "b"));
    CHECK_OK(my::io::append_file(path, "c"));

    auto read = my::io::read_file(path);
    CHECK_RESULT(read);
    CHECK_EQ(read.value(), std::string("abc"));
}

MYTEST_CASE(io_directories_and_listing) {
    const std::string root = case_path("dirs");
    Cleanup           cleanup{root};

    CHECK_OK(my::io::create_directories(my::io::join(root, "a")));
    CHECK_OK(my::io::create_directories(my::io::join(root, "b")));
    CHECK_OK(my::io::write_file(my::io::join(root, "c.txt"), "x"));

    auto names = my::io::list_dir(root);
    CHECK_RESULT(names);
    CHECK_EQ(names.value().size(), std::size_t{3});
    // 排序固定：a, b, c.txt
    CHECK_EQ(names.value()[0], std::string("a"));
    CHECK_EQ(names.value()[1], std::string("b"));
    CHECK_EQ(names.value()[2], std::string("c.txt"));

    auto bad = my::io::list_dir(case_path("no_such_dir"));
    CHECK(!bad);
    CHECK_EQ(bad.status().code(), my::Errc::NotFound);
}

MYTEST_CASE(io_remove_and_remove_all) {
    const std::string root = case_path("remove");
    Cleanup           cleanup{root};

    CHECK_OK(my::io::create_directories(my::io::join(root, "nested")));
    CHECK_OK(my::io::write_file(my::io::join(root, "nested/f.txt"), "x"));

    // 非空目录不能直接 remove
    auto st = my::io::remove(root);
    CHECK(!st);

    CHECK_OK(my::io::remove(my::io::join(root, "nested/f.txt")));
    CHECK_OK(my::io::remove(my::io::join(root, "nested")));
    CHECK(!my::io::exists(my::io::join(root, "nested")));

    CHECK_OK(my::io::remove_all(root));
    // 对不存在的路径 remove_all 不算错误（幂等）
    CHECK_OK(my::io::remove_all(root));
}

MYTEST_CASE(io_rename_and_copy) {
    const std::string src = case_path("src.txt");
    const std::string dst = case_path("dst.txt");
    Cleanup           cleanup{src};
    Cleanup           cleanup2{dst};

    CHECK_OK(my::io::write_file(src, "payload"));

    CHECK_OK(my::io::copy_file(src, dst));
    CHECK(my::io::exists(src));  // copy 不动源文件
    CHECK_OK_VALUE(dst_text, my::io::read_file(dst));
    CHECK_EQ(dst_text, std::string("payload"));

    const std::string moved = case_path("moved.txt");
    Cleanup           cleanup3{moved};
    CHECK_OK(my::io::rename(dst, moved));
    CHECK(!my::io::exists(dst));
    CHECK(my::io::exists(moved));

    // 源不存在时复制必须失败
    auto bad = my::io::copy_file(case_path("no_src.txt"), dst);
    CHECK(!bad);
}

MYTEST_CASE(io_info_reports_kind_and_size) {
    const std::string dir  = case_path("info_dir");
    const std::string file = my::io::join(dir, "f.bin");
    Cleanup           cleanup{dir};

    CHECK_OK(my::io::create_directories(dir));
    CHECK_OK(my::io::write_file(file, "12345"));

    CHECK_OK_VALUE(dir_info, my::io::info(dir));
    CHECK(dir_info.is_directory);
    CHECK(!dir_info.is_regular);

    CHECK_OK_VALUE(file_info, my::io::info(file));
    CHECK(file_info.is_regular);
    CHECK(!file_info.is_directory);
    CHECK_EQ(file_info.size, static_cast<std::uintmax_t>(5));

    auto missing = my::io::info(case_path("nope"));
    CHECK(!missing);
    CHECK_EQ(missing.status().code(), my::Errc::NotFound);
}

MYTEST_CASE(io_tempfile_raii_deletes) {
    auto made = my::io::TempFile::create();
    CHECK_RESULT(made);

    std::string path;
    {
        auto temp = std::move(made.value());
        path      = temp.path();
        CHECK(!path.empty());
        CHECK(my::io::exists(path));  // 创建时就已占位
        CHECK_OK(temp.write("temp data"));
        CHECK_OK_VALUE(text, my::io::read_file(path));
        CHECK_EQ(text, std::string("temp data"));
    }  // temp 析构
    CHECK(!my::io::exists(path));
}

MYTEST_CASE(io_tempfile_release_keeps_file) {
    const std::string kept = case_path("kept.tmp");
    Cleanup           cleanup{kept};

    auto made = my::io::TempFile::create();
    CHECK_RESULT(made);
    auto temp = std::move(made.value());
    CHECK_OK(temp.write("keep me"));

    const std::string path = temp.release();
    CHECK(!path.empty());
    CHECK(temp.path().empty());   // release 之后不再持有
    CHECK(my::io::exists(path));

    // 自己搬走再删掉，别在 build 目录里留垃圾
    CHECK_OK(my::io::rename(path, kept));
    CHECK_OK_VALUE(text, my::io::read_file(kept));
    CHECK_EQ(text, std::string("keep me"));
}

MYTEST_CASE(io_absolute_path) {
    const std::string path = case_path("abs.txt");
    Cleanup           cleanup{path};
    CHECK_OK(my::io::write_file(path, "x"));

    auto abs = my::io::absolute(path);
    CHECK_RESULT(abs);
    CHECK(my::io::exists(abs.value()));
    // 绝对路径必然含分隔符
    CHECK(abs.value().find('/') != std::string::npos || abs.value().find('\\') != std::string::npos);
}

// UTF-8 路径：Windows 上如果没把 UTF-8 显式转成 UTF-16（而依赖 ANSI 代码页），
// 中文路径会直接坏掉 —— 这条用例就是那个 bug 的守门员。
MYTEST_CASE(io_utf8_path_roundtrip) {
    const std::string base = my::io::join(my::io::current_dir(), "io_utf8_测试_dir");
    (void)my::io::remove_all(base);

    CHECK(my::io::create_directories(base).ok());
    CHECK(my::io::is_directory(base));

    const std::string file = my::io::join(base, "文件-测试.txt");
    CHECK(my::io::write_file(file, "hello 中文").ok());
    CHECK(my::io::exists(file));

    auto rd = my::io::read_file(file);
    CHECK_OK_VALUE(text, rd);
    CHECK_EQ(text, std::string("hello 中文"));

    // 列目录返回的名字必须还是 UTF-8（不能变成 ANSI 乱码）
    auto entries = my::io::list_dir(base);
    CHECK(entries.ok());
    if (entries.ok()) {
        bool found = false;
        for (const auto& e : entries.value()) {
            if (e == "文件-测试.txt") found = true;
        }
        CHECK(found);
    }

    // 路径拆分同样不能丢掉非 ASCII
    CHECK_EQ(my::io::filename(file), std::string("文件-测试.txt"));
    CHECK_EQ(my::io::extension(file), std::string(".txt"));

    CHECK(my::io::remove_all(base).ok());
    CHECK(!my::io::exists(base));
}