// SPDX-License-Identifier: Apache-2.0
// Does a commanded move land exactly, and do e-stop and the limits actually
// stop it?
`timescale 1ns/1ps
module tb_motion_axis;
	localparam CLK_HZ = 1_000_000;

	reg clk = 0, rst = 1, enable = 1, go = 0, stop = 0, estop = 0;
	reg lim_cw = 0, lim_ccw = 0;
	reg signed [31:0] target = 0;
	reg [31:0] vmax = 2000, accel = 8000;

	wire signed [31:0] pos;
	wire moving, at_target, lim_hit, estop_hit, step_pulse;
	wire [3:0] phase;

	integer fails = 0;
	integer vmax_seen;

	motion_axis #(.CLK_HZ(CLK_HZ), .VMIN(100)) dut (
		.clk(clk), .rst(rst), .enable(enable),
		.target(target), .go(go), .stop(stop), .estop(estop),
		.vmax(vmax), .accel(accel),
		.lim_cw(lim_cw), .lim_ccw(lim_ccw),
		.pos(pos), .moving(moving), .at_target(at_target),
		.lim_hit(lim_hit), .estop_hit(estop_hit),
		.phase(phase), .step_pulse(step_pulse)
	);

	always #500 clk = ~clk;

	always @(posedge clk)
		if (dut.vel > vmax_seen) vmax_seen = dut.vel;

	task expect(input [255:0] what, input signed [31:0] got,
	            input signed [31:0] want);
		begin
			if (got === want)
				$display("PASS  %0s = %0d", what, got);
			else begin
				$display("FAIL  %0s = %0d, want %0d", what, got, want);
				fails = fails + 1;
			end
		end
	endtask

	task do_move(input signed [31:0] t);
		begin
			target = t; vmax_seen = 0;
			@(posedge clk) go = 1;
			@(posedge clk) go = 0;
			wait (!moving);
			@(posedge clk);
		end
	endtask

	initial begin
		#2000 rst = 0; #2000;

		/* 1. A long move: must land exactly and must reach vmax. */
		do_move(1000);
		expect("long move pos", pos, 1000);
		expect("at_target", at_target, 1);
		if (vmax_seen == vmax) $display("PASS  reached vmax (%0d)", vmax_seen);
		else begin $display("FAIL  peak vel %0d, want %0d", vmax_seen, vmax);
			fails = fails + 1; end

		/* 2. Reverse, and a move too short to reach vmax (triangular). */
		do_move(940);
		expect("reverse move pos", pos, 940);
		if (vmax_seen < vmax) $display("PASS  short move stayed under vmax (%0d)", vmax_seen);
		else begin $display("FAIL  short move hit vmax"); fails = fails + 1; end

		/* 3. A single step. */
		do_move(941);
		expect("single step pos", pos, 941);

		/* 4. go with nothing to do. */
		do_move(941);
		expect("null move pos", pos, 941);

		/* 5. e-stop mid-move: must halt, and within a few clocks. */
		target = 5000;
		@(posedge clk) go = 1;
		@(posedge clk) go = 0;
		#20_000_000;			/* 20 ms in */
		if (!moving) begin $display("FAIL  move ended too early"); fails = fails + 1; end
		estop = 1;
		@(posedge clk); @(posedge clk);
		expect("estop stopped it", moving, 0);
		expect("estop_hit latched", estop_hit, 1);
		begin : estop_hold
			integer p; p = pos;
			#5_000_000;
			expect("no steps after estop", pos, p);
		end
		estop = 0;

		/* 6. A limit blocks motion into it but not away from it. */
		lim_cw = 1;
		do_move(pos + 500);
		expect("limit blocked the move", moving, 0);
		expect("lim_hit latched", lim_hit, 1);
		begin : lim_back
			integer p; p = pos;
			do_move(p - 200);
			expect("backing off the limit works", pos, p - 200);
		end
		lim_cw = 0;

		/* 7. Ramped stop lands somewhere short, with the axis idle. */
		target = pos + 5000;
		@(posedge clk) go = 1;
		@(posedge clk) go = 0;
		#20_000_000;
		@(posedge clk) stop = 1;
		@(posedge clk) stop = 0;
		wait (!moving);
		if (pos != target) $display("PASS  ramped stop halted short (pos %0d of %0d)", pos, target);
		else begin $display("FAIL  ramped stop reached the target"); fails = fails + 1; end

		/* 8. Coils de-energize when disabled. */
		enable = 0; #2000;
		expect("phase off when disabled", phase, 0);

		if (fails == 0) $display("tb_motion_axis: ALL PASS");
		else            $display("tb_motion_axis: %0d FAILURE(S)", fails);
		$finish;
	end

	initial begin
		#4_000_000_000;
		$display("tb_motion_axis: TIMEOUT");
		$finish;
	end
endmodule
