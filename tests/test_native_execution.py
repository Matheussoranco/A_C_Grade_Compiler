"""Compile, assemble, link and check output; assembly text alone is insufficient."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, cwd=ROOT):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, f"{args}:\n{result.stdout}\n{result.stderr}"
    return result.stdout


def execute(source, directory, optimization):
    assembly = directory / "program.asm"
    obj = directory / "program.o"
    executable = directory / "program"
    run([str(ROOT / "acc"), str(source), "-o", str(assembly), f"-O{optimization}"])
    text = assembly.read_text()
    run([str(ROOT / "acc"), str(source), "-o", str(assembly), f"-O{optimization}"])
    assert assembly.read_text() == text, "Assembly depends on process memory addresses"
    run(["nasm", "-felf64", str(assembly), "-o", str(obj)])
    run(["gcc", str(obj), str(ROOT / "runtime/runtime.o"), "-lm", "-o", str(executable)])
    return run([str(executable)], cwd=directory)


def main():
    run(["make", "all", "runtime"])
    for optimization in (0, 1, 2):
        with tempfile.TemporaryDirectory(prefix="acc-native-") as name:
            directory = Path(name)
            assert execute(ROOT / "examples/hello.ac", directory, optimization) == "Hello, World!\n"
            fibonacci = execute(ROOT / "examples/fibonacci.ac", directory, optimization)
            expected = [0, 1]
            for _ in range(18):
                expected.append(sum(expected[-2:]))
            pairs = [(int(n), int(v)) for n, v in re.findall(r"fib\((\d+)\) = (\d+)", fibonacci)]
            assert pairs == list(enumerate(expected[:15])) + list(enumerate(expected))
            primes = execute(ROOT / "examples/primes.ac", directory, optimization)
            assert "pi(100)   = 25" in primes and "pi(1000)  = 168" in primes and "pi(10000) = 1229" in primes
            assert "gcd(48, 18) = 6" in primes and "gcd(100, 75) = 25" in primes
            repeated = directory / "repeated.ac"
            repeated.write_text('extern fn printf(fmt: *char, ...) -> i32;\n'
                                'fn echo() -> i32 { printf("same\\n"); return 0; }\n'
                                'fn main() -> i32 { printf("same\\n"); echo(); return 0; }\n')
            assert execute(repeated, directory, optimization) == "same\nsame\n"
    print("Native execution and deterministic assembly passed at O0/O1/O2.")


if __name__ == "__main__":
    main()
