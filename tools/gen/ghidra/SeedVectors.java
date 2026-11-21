// SeedVectors.java -- Ghidra headless pre-script for a raw 68020 program image loaded at 0: the exception vector table is the only
// thing that says where code is, so every vector that points into the image becomes an entry point (and a function) before the
// auto-analysis runs. Vector 1 is the reset PC; the autovectors (25-31) are the interrupt handlers.
// @category Namco22
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.SourceType;

public class SeedVectors extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        long size = mem.getSize();
        int n = 0;
        // the table ends where the code starts: a game may put its handlers inside the 0x400 bytes the table could fill (Cyber Cycles'
        // are at 0x300-0x34C), and the slots from there on are instructions, not vectors
        long table_end = 0x400;
        for (int v = 1; v < 256 && v * 4L < table_end; v++) {
            long t = mem.getInt(toAddr(v * 4L)) & 0xFFFFFFFFL;
            if (t < 8 || t >= size || (t & 1) != 0) continue;
            if (t < table_end) table_end = t;
            Address a = toAddr(t);
            if (currentProgram.getListing().getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (currentProgram.getFunctionManager().getFunctionAt(a) == null) {
                try { createFunction(a, v == 1 ? "entry_reset" : null); } catch (Exception e) { }
            }
            currentProgram.getSymbolTable().addExternalEntryPoint(a);
            n++;
        }
        println("SeedVectors: " + n + " vectors into the image");
    }
}
