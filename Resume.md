Zizhen Liu (Lance)
+61 432354832
lzz288898@gmail.com
LinkedIn
Career Objective
UNSW Master of IT graduate (UTS Software Engineering background) seeking a graduate C++ role in Sydney within GPU research and development. Motivated by a long-standing interest in PC hardware and graphics and equipped with modern C++ fundamentals plus hands-on HW/SW bring-up experience (VHDL, DMA, Linux) to build and validate accurate simulation and modelling tools.

Education
University of New South Wales (UNSW)
Master of Information Technology								          Sep 2024-Sep 2025 
Core Courses: Advanced C++ Programming (81), Database Systems Implementation (72), IT Capstone Project (85)
University of Technology Sydney (UTS)
Bachelor of Engineering (Honours) in Software Engineering					          Feb 2021-Feb 2024
Core Courses: Data Structures & Algorithms (84), Database Fundamentals (94), Systems Testing & Quality Management (90)

Tech Stack
Languages: C++, C, Python, Bash/Shell
Linux: Debian (Ubuntu), WSL, PetaLinux, Device Tree/Overlay, Linux user-space drivers (mmap/UIO)
Hardware & FPGA: VHDL, AMD/Xilinx Kria KV260 (Zynq UltraScale+ MPSoC), Vivado (ILA/VIO, Clocking Wizard), AXI4 (Lite/Stream), AXI DMA
Algorithms & Systems: Data structures & algorithms, profiling/debugging workflows, concurrency fundamentals (threads/processes)
Version Control & Collaboration: Git, Agile/Scrum, Jira, Confluence
Additional (Software): REST APIs (FastAPI/Express), React, PostgreSQL/MySQL, Docker

Project
Modern C++ Generic Directed Weighted Graph Library (GDWG)
Implemented a modern C++20 generic directed graph library with deterministic ordering, polymorphic edge types, and full unit-test coverage.
Tech stack: C++20, STL, templates, runtime polymorphism, iterators, Catch2 unit testing, Git
•	Built a generic directed graph container gdwg::graph<N, E> with value semantics, parameterised node/weight types, and enforced node uniqueness.
•	Implemented a polymorphic edge hierarchy (edge + weighted_edge / unweighted_edge) with well-defined equality and ordering semantics.
•	Enforced deterministic edge ordering (source - destination - weight; unweighted before weighted for identical endpoints) and provided a bidirectional iterator yielding (from, to, optional<weight>).
•	Developed Catch2 unit tests covering insertion, merge/replace behaviour, ordering, and iteration correctness.
PostgreSQL Extension Development (C/Linux) - Multi-attribute Linear Hashing & Query Processing
Built a page-based multi-attribute linear hashing store in C with partial-match querying and a signature pre-filter to reduce unnecessary page probes.
Tech stack: C, file/page storage, bitwise hashing (signatures), query processing, Linux, GDB
•	Implemented a signature-based pre-filter to reduce unnecessary data/overflow page probes during partial-match queries.
•	Built efficient bitwise signature generation and matching at the page level to prune non-candidate pages early in the search path.
•	Managed low-level page layouts and memory handling in C, debugging faults and validating correctness with targeted test cases.
•	Supported selection with wildcards/patterns and projection (attribute reordering), ensuring stable output formatting and query behaviour.
FPGA Audio Capture & Gain EQ on AMD Xilinx Kria KV260 (I2S - WAV, 48 kHz / 24-bit, mono)
Built an FPGA-based mono audio capture pipeline on KV260, streaming I2S MEMS mic data via DMA to Linux and producing 48 kHz/24-bit WAV (PCM) output with a configurable gain EQ extension.
Tech stack: VHDL, Vivado (Clocking Wizard, ILA/VIO), Zynq UltraScale+ MPSoC (KV260), AXI4-Lite/AXI4-Stream, AXI DMA, Device Tree Overlay, C/C++, Python, WSL.
•	Implemented a VHDL I2S receiver (mono) in PL, generating BCLK/WS via clocking + counters and aligning samples to a 48 kHz/24-bit PCM pipeline.
•	Built FIFO/BRAM buffering to absorb clock-domain and burst-transfer jitter, preventing overruns/underruns during sustained capture.
•	Designed PS–PL integration using AXI4-Lite control registers (start/stop, frame size, gain) and AXI4-Stream + DMA transfers into DDR for low-overhead streaming.
•	Completed Linux integration via Device Tree Overlay, exposing the custom audio IP/DMA for runtime deployment.
•	Delivered an AXI-Lite controlled gain/EQ stage and verified output integrity using WAV playback plus FFT/waveform checks; debugged bring-up with Vivado ILA/VIO in a WSL-based workflow.

Work Experience
Intelli New Technologies (Sydney, Australia)
IT/Business Analysis Intern									          Oct 2023–Jan 2024
•	Built and maintained Python-based data ingestion scripts to collect, validate, and normalise large volumes of semi-structured web data, improving data quality and downstream usability.
•	Diagnosed extraction failures and data inconsistencies through log-driven debugging, and iterated parsers to handle site changes and edge cases reliably.
•	Collaborated with engineers and stakeholders to clarify requirements, translate them into technical tasks, and communicate progress and risks in an Agile workflow (Jira/Confluence).
•	Supported integration between data pipelines and product features by verifying inputs/outputs and assisting with release readiness (basic monitoring and issue triage).
Chongqing Golden Lady Photography Co., Ltd (Chongqing, China)
IT Support Intern (Computer and website Maintenance)						         May 2021–Aug 2021
•	Performed PC hardware troubleshooting and OS configuration, resolving common workstation issues and improving stability for day-to-day operations.
•	Assisted with basic network/peripheral setup and documented recurring issues to speed up diagnostics.
•	Maintained production website content and layout updates with attention to operational risk, verifying changes before and after deployment.

Interests & Additional Information
Languages: Proficient in English, native in Mandarin Chinese, beginner in Japanese
Tech Enthusiast: Passionate about computer hardware; built and assembled PCs since age 14. Have a basic knowledge of Graphics API (DirectX/Vulkan/OpenGL/Metal). Also a Photography Enthusiast.
Australian Computer Society (ACS) - Member ID: 4468719
Visa Status: 485 Temporary Graduate Visa (Full working rights, valid to November 2027)
