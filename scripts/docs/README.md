# README for frontend developers who mysteriously got here

## Requirement 📋

The build was tested on Ubuntu 22.04 (and macOS Ventura 13.5 for development purposes)

❗️ Requires doxygen 1.10.0+

## Instruction 🧾

1. install dependencies: `sudo apt install make graphviz`
2. download doxygen 1.10.0+: `wget https://www.doxygen.nl/files/doxygen-1.10.0.linux.bin.tar.gz && tar -xvzf doxygen-1.10.0.linux.bin.tar.gz`
3. run cmake and perform a full build of userver to get `compile_commands.json` and run codegen
   * on cmake 3.31+, it's enough to run `cmake --build . --target codegen` in the build dir
4. in project folder run: `make docs DOXYGEN=/PATH_TO/doxygen-1.10.0/bin/doxygen BUILD_DIR=/absolute/path/to/build_dir`

## How to develop? 🛠️

After building the documentation, you can add hard links to the files to be changed. For example, I'm going to change `customdoxygen.css`:

```
frontend@PC:~/Desktop/userver$ rm docs/html/customdoxygen.css
frontend@PC:~/Desktop/userver$ ln scripts/docs/customdoxygen.css docs/html/customdoxygen.css
```

Now it is possible to use tools like [LiveServer](https://marketplace.visualstudio.com/items?itemName=ritwickdey.LiveServer)
