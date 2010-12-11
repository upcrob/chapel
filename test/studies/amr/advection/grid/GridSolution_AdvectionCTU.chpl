
use GridSolution_def;
use GridBC_def;
use GridVariable_AdvectionCTU;




//|\"""""""""""""""""""""""""""""""""""""""""""""""""|\
//| >    GridSolution.advance_AdvectionCTU method    | >
//|/_________________________________________________|/

//------------------------------------------------------------------
// This advances the GridSolution to time_requested by applying the
// CTU (corner transport upwind) method.  Mathematical content of
// the algorithm is in the method GridVariable.storeCTUOperator.
//------------------------------------------------------------------

def GridSolution.advance_AdvectionCTU(
  bc:             GridBC,
  velocity:       dimension*real,
  time_requested: real)
{

  //==== Safety check ====
  assert(current_time <= time_requested);


  //===> Calculate dt_target via CFL condition ===>
  var cfl: [dimensions] real,
      dt_target:        real,
      dt:               real;

  [d in dimensions] cfl(d) = grid.dx(d) / abs(velocity(d));
  (dt_target,) = minloc reduce(cfl, dimensions);
  dt_target *= 0.95;
  //<=== Calculate dt_target via CFL condition <===
  

  
  //===> Time-stepping loop ===>
  while (current_time < time_requested) do {

    //==== Adjust the time step to hit time_requested if necessary ====
    if (current_time + dt_target > time_requested) then
      dt = time_requested - current_time;
    else
      dt = dt_target;
    writeln("Taking step of size dt=", dt, " to time ", current_time+dt, ".");


    //==== Update solution ====
    step_AdvectionCTU(bc, velocity, dt);
          
  }
  //<=== Time-stepping loop <===

}
// /|""""""""""""""""""""""""""""""""""""""""""/|
//< |    GridSolution.advance_AdvectionCTU    < |
// \|__________________________________________\|





//|\"""""""""""""""""""""""""""""""""""""""|\
//| >    GridSolution.step_AdvectionCTU    | >
//|/_______________________________________|/

//-------------------------------------------------------------
// The output of the oprator will be stored in q.old, and then
// q.old and q.current will be swapped.
//-------------------------------------------------------------

def GridSolution.step_AdvectionCTU(
  bc:       GridBC,
  velocity: dimension*real,
  dt:       real)
{

  //==== Apply physical BC ====
  bc.apply(current_data, current_time);

  //==== Update solution data ====
  old_data.storeCTUOperator(current_data, velocity, dt);
  old_data <=> current_data;
  
  //==== Update solution structure ====
  old_time = current_time;
  current_time += dt;

}
// /|""""""""""""""""""""""""""""""""""""""/|
//< |   GridSolution.step_AdvectionCTU    < |
// \|______________________________________\|