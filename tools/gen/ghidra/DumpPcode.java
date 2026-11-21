// DumpPcode.java -- Ghidra headless post-script: every function's instructions with their RAW p-code, as text for tools/gen/lift68k.py.
//
//   analyzeHeadless <proj dir> <proj> -import prog.bin -loader BinaryLoader -processor 68000:BE:32:MC68020 \
//       -scriptPath tools/gen/ghidra -preScript SeedVectors.java -postScript DumpPcode.java out.pcode
//
// Lines:
//   F <entry> <name>                         a function (its body: every instruction until the next F)
//   I <addr> <length> <fallthrough or -> <mnemonic and operands>
//   P <OPCODE> <out or -> <in> <in> ...      one p-code op; a varnode is space:offset:size (hex offset), e.g. register:20:4
//   U <index> <name>                         the CALLOTHER user-op names (once, at the top)
//   R <offset> <size> <name>                 the registers (once, at the top)
// @category Namco22
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;
import java.io.PrintWriter;

public class DumpPcode extends GhidraScript {
    private String vn(Varnode v) {
        return v.getAddress().getAddressSpace().getName() + ":" + Long.toHexString(v.getOffset()) + ":" + v.getSize();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "out.pcode";
        PrintWriter w = new PrintWriter(out, "UTF-8");
        int nu = currentProgram.getLanguage().getNumberOfUserDefinedOpNames();
        for (int i = 0; i < nu; i++) w.println("U " + i + " " + currentProgram.getLanguage().getUserDefinedOpName(i));
        for (Register r : currentProgram.getLanguage().getRegisters())
            w.println("R " + Long.toHexString(r.getOffset()) + " " + r.getMinimumByteSize() + " " + r.getName());
        int nf = 0, ni = 0;
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
        while (fi.hasNext()) {
            Function f = fi.next();
            if (f.isExternal()) continue;
            AddressSetView body = f.getBody();
            InstructionIterator ii = currentProgram.getListing().getInstructions(body, true);
            if (!ii.hasNext()) continue;
            w.println("F " + Long.toHexString(f.getEntryPoint().getOffset()) + " " + f.getName());
            nf++;
            while (ii.hasNext()) {
                Instruction in = ii.next();
                Address ft = in.getFallThrough();
                w.println("I " + Long.toHexString(in.getAddress().getOffset()) + " " + in.getLength() + " "
                        + (ft == null ? "-" : Long.toHexString(ft.getOffset())) + " " + in.toString());
                ni++;
                for (PcodeOp op : in.getPcode()) {
                    StringBuilder sb = new StringBuilder("P ");
                    sb.append(op.getMnemonic()).append(' ');
                    sb.append(op.getOutput() == null ? "-" : vn(op.getOutput()));
                    for (Varnode v : op.getInputs()) sb.append(' ').append(vn(v));
                    w.println(sb);
                }
            }
        }
        // instructions that belong to no function (Ghidra disassembled them but made no function): each contiguous run is dumped as a
        // function of its own, named orphan_<addr>, so the lifter has them (they are reached by computed jumps or table calls)
        InstructionIterator all = currentProgram.getListing().getInstructions(true);
        int orphans = 0; long expect = -1;
        while (all.hasNext()) {
            Instruction in = all.next();
            if (currentProgram.getFunctionManager().getFunctionContaining(in.getAddress()) != null) continue;
            orphans++;
            long a = in.getAddress().getOffset();
            if (a != expect) { w.println("F " + Long.toHexString(a) + " orphan_" + Long.toHexString(a)); nf++; }
            expect = a + in.getLength();
            Address ft = in.getFallThrough();
            w.println("I " + Long.toHexString(a) + " " + in.getLength() + " " + (ft == null ? "-" : Long.toHexString(ft.getOffset())) + " " + in.toString());
            for (PcodeOp op : in.getPcode()) {
                StringBuilder sb = new StringBuilder("P ");
                sb.append(op.getMnemonic()).append(' ');
                sb.append(op.getOutput() == null ? "-" : vn(op.getOutput()));
                for (Varnode v : op.getInputs()) sb.append(' ').append(vn(v));
                w.println(sb);
            }
        }
        w.close();
        println("DumpPcode: " + nf + " functions, " + ni + " instructions, " + orphans + " orphan instructions -> " + out);
    }
}
