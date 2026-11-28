// FixSwitches.java -- Ghidra headless post-script (before DumpPcode.java): a switch's jump table the analysis missed, or missed a case of.
// Arg: a file of lines "<jr address> <case address> <case address> ..." (hex; a game's tools/scan_switches.py writes it). Each case is
// disassembled, added to the jr as a computed-jump target, and the function holding the jr is grown over it -- so the cases stay inside
// that function and a jump to one is a local goto in the translated C. (A seed would make the case a function of its own: a loop running
// through the switch would then cross a function boundary on every turn.) A case already inside another function is left alone.
// First used by Downhill Bikers: 0x8004B73C's case 0 (0x8004B76C) was never disassembled.
// @category Namco22
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;

public class FixSwitches extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { println("FixSwitches: no table file"); return; }
        int tables = 0, added = 0, grown = 0;
        for (String line : java.nio.file.Files.readAllLines(new java.io.File(args[0]).toPath())) {
            String[] p = line.trim().split("\\s+");
            if (p.length < 2 || p[0].startsWith("#")) continue;
            Address jr = toAddr(Long.parseLong(p[0], 16));
            Instruction ji = currentProgram.getListing().getInstructionAt(jr);
            Function f = currentProgram.getFunctionManager().getFunctionContaining(jr);
            if (ji == null || f == null) continue;
            tables++;
            boolean changed = false;
            for (int k = 1; k < p.length; k++) {
                Address c = toAddr(Long.parseLong(p[k], 16));
                Function other = currentProgram.getFunctionManager().getFunctionContaining(c);
                if (other != null && !other.equals(f)) continue;
                if (currentProgram.getListing().getInstructionAt(c) == null) { new DisassembleCommand(c, null, true).applyTo(currentProgram, monitor); added++; }
                boolean has = false;
                for (ghidra.program.model.symbol.Reference r : ji.getReferencesFrom()) if (r.getToAddress().equals(c)) has = true;
                if (!has) { ji.addMnemonicReference(c, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED); changed = true; }
            }
            if (changed && CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor)) grown++;
        }
        println("FixSwitches: " + tables + " tables, " + added + " cases disassembled, " + grown + " functions grown");
    }
}
