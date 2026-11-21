// Probe.java -- Ghidra headless script: what the listing holds at some addresses (arguments: hex addresses), for finding out why
// a routine was not disassembled.
// @category Namco22
import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class Probe extends GhidraScript {
    @Override
    public void run() throws Exception {
        PseudoDisassembler pseudo = new PseudoDisassembler(currentProgram);
        for (String s : getScriptArgs()) {
            Address a = toAddr(Long.parseLong(s, 16));
            println("Probe " + s + ": validSubroutine=" + pseudo.isValidSubroutine(a, true));
            for (int k = -8; k <= 10; k++) {
                Address x = a.add(k);
                Instruction in = currentProgram.getListing().getInstructionAt(x);
                Data d = currentProgram.getListing().getDefinedDataAt(x);
                if (in != null) {
                    StringBuilder sb = new StringBuilder("Probe   " + x + " INSTR " + in + " len " + in.getLength() + " ft " + in.getFallThrough());
                    for (Reference r : getReferencesTo(x)) sb.append(" <-").append(r.getFromAddress()).append(":").append(r.getReferenceType());
                    println(sb.toString());
                }
                if (d != null) {
                    StringBuilder sb = new StringBuilder("Probe   " + x + " DATA " + d + " len " + d.getLength());
                    for (Reference r : getReferencesTo(x)) sb.append(" <-").append(r.getFromAddress()).append(":").append(r.getReferenceType());
                    println(sb.toString());
                }
            }
            println("Probe   function containing: " + currentProgram.getFunctionManager().getFunctionContaining(a));
        }
    }
}
