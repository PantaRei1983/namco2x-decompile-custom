// SeedMips.java -- Ghidra headless pre-script for a System 23 MIPS game program (prog.bin loaded at 0x80000000): the R4650's exception
// vectors (0x80000000 TLB refill, 0x80000080 XTLB, 0x80000100 cache error, 0x80000180 general) and the ELF entry are the only things that
// say where code is; each becomes a function before the auto-analysis runs. Args: a seeds file or hex addresses, and block:<hex addr>:<file>
// to map another image (the boot monitor at 0xBFC00000, whose reset vector 0xBFC00000 and BEV exception vector 0xBFC00380 are seeded too).
// @category Namco22
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class SeedMips extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        java.util.List<Long> seeds = new java.util.ArrayList<>();
        for (long v : new long[] { 0x80000000L, 0x80000080L, 0x80000100L, 0x80000180L }) seeds.add(v);
        for (String s : args) {
            if (s.startsWith("block:")) {
                String[] p = s.split(":", 3);
                long base = Long.parseLong(p[1], 16);
                byte[] b = java.nio.file.Files.readAllBytes(new java.io.File(p[2]).toPath());
                currentProgram.getMemory().createInitializedBlock("boot", toAddr(base), new java.io.ByteArrayInputStream(b), b.length, monitor, false);
                seeds.add(base); seeds.add(base + 0x380);
                continue;
            }
            java.io.File f = new java.io.File(s);
            if (f.isFile()) { for (String l : java.nio.file.Files.readAllLines(f.toPath())) { l = l.trim(); if (!l.isEmpty() && !l.startsWith("#")) seeds.add(Long.parseLong(l.split("\s+")[0], 16)); } }
            else seeds.add(Long.parseLong(s, 16));
        }
        int n = 0;
        for (long v : seeds) {
            Address a = toAddr(v);
            if (currentProgram.getMemory().getBlock(a) == null) continue;
            if (currentProgram.getListing().getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (currentProgram.getFunctionManager().getFunctionAt(a) == null) { try { createFunction(a, null); } catch (Exception e) { } }
            currentProgram.getSymbolTable().addExternalEntryPoint(a);
            n++;
        }
        println("SeedMips: " + n + " seeds");
    }
}
