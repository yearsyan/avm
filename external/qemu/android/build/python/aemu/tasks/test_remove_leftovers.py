import subprocess
import sys
import time
from pathlib import Path
from tempfile import TemporaryDirectory

from aemu.tasks.remove_leftovers import RemoveLeftoverTask

def is_process_running(pid: int) -> bool:
    """Checks if a process with the given PID is running on Windows."""
    try:
        result = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}"],
            capture_output=True,
            text=True,
            check=True,
        )
        # tasklist's output will contain the PID if the process is found.
        return str(pid) in result.stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False

def test_remove_leftovers():
    """
    A simple test for the RemoveLeftoverTask.

    1. Creates a temporary directory.
    2. Creates a simple 'sleeper.py' script inside it.
    3. Runs the 'sleeper.py' script as a background process.
    4. Runs the RemoveLeftoverTask to kill the process.
    5. Verifies that the process was terminated.
    """
    with TemporaryDirectory() as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        print(f"Created temporary directory: {temp_dir}")

        # 1. Create a dummy python script that runs for a while
        sleeper_script_path = temp_dir / "sleeper.py"
        sleeper_script_path.write_text(
            "import time\n"
            "print(f'Sleeper process started, sleeping for 300 seconds...')\n"
            "time.sleep(300)\n"
        )
        print(f"Created sleeper script: {sleeper_script_path}")

        # 2. Run the script in a new background process
        process = subprocess.Popen(
            [sys.executable, str(sleeper_script_path)],
            cwd=str(temp_dir),
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
        )
        print(f"Started sleeper process with PID: {process.pid}")

        # Give it a moment to ensure it's fully running
        time.sleep(2)

        if not is_process_running(process.pid):
            print("Error: Sleeper process failed to start.")
            return

        print(f"Sleeper process {process.pid} is running.")

        # 3. Run the task to clean up leftovers
        print("\nRunning RemoveLeftoverTask...")
        task = RemoveLeftoverTask(target="windows", destination=temp_dir)
        task.do_run()

        # 4. Check if the process was terminated
        print("\nVerifying if process was terminated...")
        # Give taskkill a moment to work
        time.sleep(2)

        if not is_process_running(process.pid):
            print(f"Success! Process {process.pid} was terminated by the task.")
        else:
            print(f"Failure! Process {process.pid} is still running.")
            # Clean up by killing the process manually to not leave it hanging
            print(f"Manually terminating process {process.pid}.")
            subprocess.run(["taskkill", "/F", "/PID", str(process.pid)], check=False)

if __name__ == "__main__":
    test_remove_leftovers()
