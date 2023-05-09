## Name

**dispatch** - Run a sequence of commands with a determined concurrency

## Synopsis

Normal mode: `dispatch <start> <end> <conc> <command> [options...]`

_Slurm_ mode: `dispatch <offset_array> <offset_index> <num> <command> [options...]`

## Description

**dispatch** has two modes:
- In normal mode, the user provides the start and end numbers of the sequence, along with the number of elements to run concurrently.
- In _slurm_ mode, the user provides the start numbers of both the Slurm array and sequence, plus the number of elements that are to be run by each instance of the array job.

_Slurm_ mode is activated if the `SLURM_JOB_ID` environment variable is present.
_Slurm_ mode is intended to allow running more elements that is normally permitted by Slurm configuration,
while at the same time allowing for elements to run concurrently (for instance when hyperthreading is enabled).

For each element, **dispatch** executes the command and arguments provided in its call, with the element index provided as last argument; that is
`command [options...] <index>`

## Arguments

In normal mode, the arguments are:
- `start`: The index of the first element to run
- `end`: The index of the last element to run
- `conc`: The number of elements to run concurrently
- `command [options...]`: The command to run for each element; the index will be appended as last argument

In _slurm_ mode, the arguments are:
- `offset_array`: The index of SLURM array job that will call dispatch
- `offset_index`: The index of the first element to run
- `num`: The number of elements to execute by each SLURM array job instance
- `command [options...]`: The command to run for each element; the index will be appended as last argument

In _slurm_ mode, the number of elements to run concurrently is retrived from the `SLURM_CPUS_ON_NODE` environment variable,
while the array index is retrived from the `SLURM_ARRAY_TASK_ID` environment variable. The correspondance between Slurm job array index and sequence elements is:
- Slurm array index `offset_array` will run indexes from `offset_index` to `offset_index + num - 1`.
- Slurm array index `offset_array + 1` will run indexes from `offset_index + num` to `offset_index + (2 * num) - 1`.
- and so on

## Run-time

In normal mode, **dispatch** accepts basic commands in the standard input to modify some of the values provided in arguments. These are:
- `end <value>` or `e <value>`: override the value of the `end` argument of normal mode; the value must not be lower than the index of the last element that was started
- `conc <value>` or `c <value>`: override the value of the `conc` argument of normal mode; such that no new element is started while the number . Elements already running are never terminated.

## Output

**dispatch** forwards to output of each element to the caller, with each line prepended with `Child <index>:`, where `<index>` is the index of the element that generated the output.

## Example

Calling `dispatch 12 18 1 echo test` (normal mode) will produce:
```
Child 12: test 12
Child 13: test 13
Child 14: test 14
Child 15: test 15
Child 16: test 16
Child 17: test 17
Child 18: test 18
```
