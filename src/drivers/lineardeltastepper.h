/* The MIT License (MIT)
 *
 * Copyright (c) 2014 Colin Wallace
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* 
 * Printipi/drivers/lineardeltastepper.h
 * 
 * LinearDeltaStepper implements the AxisStepper interface for (rail-based) Delta-style robots like the Kossel
 */

/* Raspberry Pi float performance can be found here: http://www.raspberrypi.org/forums/viewtopic.php?t=7336
  float +,-,*: 2 cycles
  float /: 32 cycles (same for doubles)
  float sqrt: 48 cycles (same for doubles)
  float atan2: ?
    Could be approximated well in ~100 cycles (for float; more for double)
    This website has some efficient trig implementations: http://http.developer.nvidia.com/Cg/atan2.html
*/

/* Useful kinematics document: https://docs.google.com/viewer?a=v&pid=forums&srcid=MTgyNjQwODAyMDkxNzQxMTUwNzIBMDc2NTg4NjQ0MjUxMTE1ODY5OTkBdmZiejRRR2phZjhKATAuMQEBdjI
 * 
 *Locations of towers:
 * Each tower is situated at the corner of an equilateral triangle,
 *   at a distance 'r' from the center of the triangle.
 * Tower A is at (rsin(0)  , rcos(0)  ) = (0           , r     )
 * Tower B is at (rsin(120), rcos(120)) = (sqrt(3)/2 r , -1/2 r)
 * Tower C is at (rsin(240), rcos(240)) = (-sqrt(3)/2 r, -1/2 r)
 *
 * Top-down view:
 *         A
 *        /|\
 *       / | \
 *      /  |  \
 *     /  r|   \
 *    /    .    \
 *   /   (0,0)   \
 *  /             \
 * /               \
 *C-----------------B
 *
 * Front-face view:
 *
 * C         A        B
 * |\       /|     __/|
 * | \ L   / |  __/   |
 * |  \   /  |_/      |
 * |   \./__/|        |
 * | (x,y,z) |        |
 * |                  |
 * |                  |
 *
 * The '.' represents the effector.
 * Each tower has a rod of fixed-length 'L' connecting to the effector. The other end of the rod is connected to a carriage that slides up and down the axis. The connection points allow the rot to pivot freely.
 * The height of the carriage above the bed is indicated by 'A' for the A carriage, 'B' for the B carriage, and 'C' for the C carriage.
 *
 * This gives us the following equations for relating A, B, C to x, y, z:
 * (A-z)^2 + (x-rsin(0)  )^2 + (y-rcos(0)  )^2 = L^2
 * (B-z)^2 + (x-rsin(120))^2 + (y-rcos(120))^2 = L^2
 * (C-z)^2 + (x-rsin(240))^2 + (y-rcos(240))^2 = L^2
 *
 * We can solve this system for A, B, and C to get the "Inverse Kinematics":
 * A = z + sqrt(L^2 - (y-rcos(0)  )^2 - (x-rsin(0)  )^2)
 * B = z + sqrt(L^2 - (y-rcos(120))^2 - (x-rsin(120))^2)
 * C = z + sqrt(L^2 - (y-rcos(240))^2 - (x-rsin(240))^2)
 *
 * If we want to move linearly along x,y,z at a constant velocity (acceleration will be introduced later):
 *   let x(t) = x0 + vx t
 *       y(t) = y0 + vy t
 *       z(t) = z0 + vz t
 *
 * Then A(t) = z0 + vz*t + sqrt( L^2 - (y0 + vy*t - rcos(0)  )^2 + (x0 + vx*t - rsin(0)  )^2 )
 *      B(t) = z0 + vz*t + sqrt( L^2 - (y0 + vy*t - rcos(120))^2 + (x0 + vx*t - rsin(120))^2 )
 *      C(t) = z0 + vz*t + sqrt( L^2 - (y0 + vy*t - rcos(240))^2 + (x0 + vx*t - rsin(240))^2 )
 *
 * Now, if we are at A=A0, we want to find the next time at which we'll be at A=A0 +/- 1,
 *   That is, the time at which A is stepped exactly +/-1 step.
 *   It is important to note that along a linear movement, it is possible for dA/dt to change sign. That is, a given carriage might be moving up at the beginning of the line, and moving down at the end of the line.
 *
 * So, let A0 = A(t=0).
 * Then we want to solve A(t) = A0 + s for t, where s is the number of steps from A0.
 *   This will allow us to test t(A0-1) and t(A0+1) to determine the direction and time to take our first step. If it is positive, then we will next test t(A0+1-1) and t(A0+1+1), and so on, to form our path.
 * To make this generic, we will replace A, B, C with D, and cos(0), cos(120), ... with cos(w).
 *   That way we only have to solve one axis, and can then obtain results for all axes.
 * 
 * D0 + s = z0 + vz*t + sqrt( L^2 - (y0 + vy*t - rcos(w))^2 + (x0 + vx*t - rsin(w))^2 )
 * Expand:
 *   (D0 + s - z0 - vz*t)^2 = L^2 - ((y0 - rcos(w)) + vy*t)^2 + ((x0 - rsin(w)) + vx*t)^2
 *
 *   ((D0 + s - z0) - vz*t)^2 - L^2 = (y0-rcos(w))^2 + 2vy*t*(y0 - rcos(w)) + vy*vy*t*t   +   (x0-rsin(w))^2 + 2vx*t*(x0-rsin(w)) + vx*vx*t
 *
 *   (D0 + s - z0)^2 - 2(D0 + s - z0)*vz*t + vz*vz*t*t - L^2 = (y0-rcos(w))^2 + 2vy*t*(y0 - rcos(w)) + vy*vy*t*t   +   (x0-rsin(w))^2 + 2vx*t*(x0-rsin(w)) + vx*vx*t
 *
 * This looks like a quadratic equation of t; group the powers of t:
 *   0 = -(D0 + s - z0)^2 + 2(D0 + s - z0)*vz*t - vz*vz*t*t + L^2 + (y0-rcos(w))^2 + 2vy*t*(y0 - rcos(w)) + vy*vy*t*t   +   (x0-rsin(w))^2 + 2vx*t*(x0-rsin(w)) + vx*vx*t
 *
 *   0 = t^2*(-vz^2 + vy^2 +vx^2)  +  t*(2(D0 + s - z0)*vz + 2vy*(y0 - rcos(w)) + 2vx*(x0-rsin(w)))  +  (-(D0 + s - z0)^2 + L^2 + (y0-rcos(w))^2 + (x0-rsin(w))^2)
 *
 * Thus, 0 = a t^2 + b t + c, where
 *   a = -vz^2 + vy^2 +vx^2
 *   b = 2(D0 + s - z0)*vz + 2vy*(y0 - rcos(w)) + 2vx*(x0-rsin(w))
 *   c = -(D0 + s - z0)^2 + L^2 + (y0-rcos(w))^2 + (x0-rsin(w))^2
 * So t = [-b +/- sqrt(b^2 - 4 ac)] / [2a]  according to the quadratic formula
 *
 * There are two solutions; both may be valid, but have different meanings. If one solution is at a time in the past, then it's just a projection of the current path into the past. If both solutions are in the future, then pick the nearest one; it means that there are two points in this path where the carriage should be at the same spot. This happens, for example, when the effector nears a tower for half of the line-segment, and then continues past the tower, so that the carriage movement is (pseudo) parabolic.
 * The above quadratic solution is used in LinearDeltaStepper::_nextStep(), although it has been optimized.
 *
 * Note: all motion in this file is planned at a constant velocity. Cartesian-space acceleration is introduced by a post-transformation of the step times applied elsewhere in the motion planning system.
 *
 *
 *
 *
 *
 *If we want to move in an ARC along x,y at a constant velocity (acceleration will be introduced later):
 *   let x(t) = x0 + q cos(u*t)
 *       y(t) = y0 + q sin(u*t)
 *       z(t) = z0
 *   or, p(t) = <x0, y0, z0> + <qcos(u*t), qsin(u*t), 0>
 *   Then, in order to apply a phase to the rotation (i.e., not start at (x0+q, y0)), we just apply a rotation matrix (based about the z-axis) to the second parameter:
 *   p(t) = <x0, y0, z0> + Rz <qcos(u*t), qsin(u*t), 0>
 *   In order to support 3d rotations, we add Rx and Ry matrices as well. Thus,
 *   p(t) = <x0, y0, z0> + Rx Ry Rz q<cos(u*t), sin(u*t), 0>
 *
 *   Note: Rz = [1     0     0
 *               0     cosa  -sina
 *               0     sina  cosa]
 *         Ry = [cosb  0     sinb
 *               0     1     0
 *               -sinb 0     cosb]
 *         Rx = [cosc  -sinc 0
 *               sinc  cosc  0
 *               0     0     1]
 *
 *   Note: G2/G3 can specify CW or CCW motion. This is done by adding 180* to either b or c.
 *
 * Mathematica Notation:
 *   R = {{1,0,0},{0,Cos[a],-Sin[a]},{0,Sin[a],Cos[a]}} . {{Cos[b],0, Sin[b]},{0,1,0},{-Sin[b],0,Cos[b]}} . {{Cos[c],-Sin[c],0},{Sin[c],Cos[c],0},{0,0,1}} = {{Cos[b] Cos[c], -(Cos[b] Sin[c]), Sin[b]}, {Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c], Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c], -(Cos[b] Sin[a])}, {-(Cos[a] Cos[c] Sin[b]) + Sin[a] Sin[c], Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c], Cos[a] Cos[b]}}
 * Then, p(t) = {{x0}, {y0}, {z0}} + q{{Cos[b] Cos[c] Cos[t u] - Cos[b] Sin[c] Sin[t u]}, 
 {Cos[t u] (Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c]) + (Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c]) Sin[t u]},
 {Cos[t u] (-Cos[a] Cos[c] Sin[b] + Sin[a] Sin[c]) + (Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c]) Sin[t u]}}
 *   
 * Then we substitute this information into the above derived D = z + sqrt(L^2 - (y-rcos(w))^2 - (x-rsin(w))^2), where w is tower angle and D is some axis coordinate.
 *   D(t) = z0 + q(Cos[t u] (-Cos[a] Cos[c] Sin[b] + Sin[a] Sin[c]) + (Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c]) Sin[t u])
      + Sqrt[L^2 - (y0 + q(Cos[t u] (Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c]) + (Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c]) Sin[t u]) - r Cos[w])^2 - (x0 + q(Cos[b] Cos[c] Cos[t u] - Cos[b] Sin[c] Sin[t u]) - r Sin[w])^2]
 * Solve for D(t) = D0 + s:
 *   D0 + s = z0 + q(Cos[t u] (-Cos[a] Cos[c] Sin[b] + Sin[a] Sin[c]) + (Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c]) Sin[t u])
      + Sqrt[L^2 - (y0 + q(Cos[t u] (Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c]) + (Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c]) Sin[t u]) - r Cos[w])^2 - (x0 + q(Cos[b] Cos[c] Cos[t u] - Cos[b] Sin[c] Sin[t u]) - r Sin[w])^2]
 *
 *   (D0 + s - z0 - q*Cos[t u](-Cos[a] Cos[c] Sin[b] + Sin[a] Sin[c]) - q*Sin[t u](Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c]))^2 = L^2 - (y0 + q(Cos[t u] (Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c]) + (Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c]) Sin[t u]) - r Cos[w])^2 - (x0 + q(Cos[b] Cos[c] Cos[t u] - Cos[b] Sin[c] Sin[t u]) - r Sin[w])^2
 *
 *   0 = L^2 - (D0 + s - z0 - q*Cos[t u](-Cos[a] Cos[c] Sin[b] + Sin[a] Sin[c]) - q*Sin[t u](Cos[c] Sin[a] + Cos[a] Sin[b] Sin[c]))^2 - (y0 + q(Cos[t u] (Cos[c] Sin[a] Sin[b] + Cos[a] Sin[c]) + (Cos[a] Cos[c] - Sin[a] Sin[b] Sin[c]) Sin[t u]) - r Cos[w])^2 - (x0 + q(Cos[b] Cos[c] Cos[t u] - Cos[b] Sin[c] Sin[t u]) - r Sin[w])^2
 *
 * Expand applied to above yields: 
 *   0 = -D0^2+L^2-2 D0 s-s^2-q^2 Cos[b]^2 Cos[c]^2 Cos[t u]^2-r^2 Cos[w]^2-2 D0 q Cos[a] Cos[c] Cos[t u] Sin[b]-2 q s Cos[a] Cos[c] Cos[t u] Sin[b]+2 q r Cos[c] Cos[t u] Cos[w] Sin[a] Sin[b]-q^2 Cos[a]^2 Cos[c]^2 Cos[t u]^2 Sin[b]^2-q^2 Cos[c]^2 Cos[t u]^2 Sin[a]^2 Sin[b]^2+2 q r Cos[a] Cos[t u] Cos[w] Sin[c]+2 D0 q Cos[t u] Sin[a] Sin[c]+2 q s Cos[t u] Sin[a] Sin[c]-q^2 Cos[a]^2 Cos[t u]^2 Sin[c]^2-q^2 Cos[t u]^2 Sin[a]^2 Sin[c]^2+2 q r Cos[a] Cos[c] Cos[w] Sin[t u]+2 D0 q Cos[c] Sin[a] Sin[t u]+2 q s Cos[c] Sin[a] Sin[t u]-2 q^2 Cos[a]^2 Cos[c] Cos[t u] Sin[c] Sin[t u]+2 q^2 Cos[b]^2 Cos[c] Cos[t u] Sin[c] Sin[t u]-2 q^2 Cos[c] Cos[t u] Sin[a]^2 Sin[c] Sin[t u]+2 D0 q Cos[a] Sin[b] Sin[c] Sin[t u]+2 q s Cos[a] Sin[b] Sin[c] Sin[t u]-2 q r Cos[w] Sin[a] Sin[b] Sin[c] Sin[t u]+2 q^2 Cos[a]^2 Cos[c] Cos[t u] Sin[b]^2 Sin[c] Sin[t u]+2 q^2 Cos[c] Cos[t u] Sin[a]^2 Sin[b]^2 Sin[c] Sin[t u]-q^2 Cos[a]^2 Cos[c]^2 Sin[t u]^2-q^2 Cos[c]^2 Sin[a]^2 Sin[t u]^2-q^2 Cos[b]^2 Sin[c]^2 Sin[t u]^2-q^2 Cos[a]^2 Sin[b]^2 Sin[c]^2 Sin[t u]^2-q^2 Sin[a]^2 Sin[b]^2 Sin[c]^2 Sin[t u]^2+2 q r Cos[b] Cos[c] Cos[t u] Sin[w]-2 q r Cos[b] Sin[c] Sin[t u] Sin[w]-r^2 Sin[w]^2
 *
 * FullSimplify on above gives: 
 *   0 = L^2-q^2-r^2-x0^2-y0^2-(D0+s-z0)^2+2 r y0 Cos[w]-2 q Cos[c+t u] (x0 Cos[b]+((D0+s-z0) Cos[a]+(y0-r Cos[w]) Sin[a]) Sin[b])+2 q (Cos[a] (-y0+r Cos[w])+(D0+s-z0) Sin[a]) Sin[c+t u]+2 r (x0+q Cos[b] Cos[c+t u]) Sin[w]
 *
 * Note, contains only Sin[c+t u] and Cos[c+t u] terms, and never multiplied together. May therefore be possible to solve with arctan. Mathematica says that if a*sin(x) + b*cos(x) +c = 0, then x = 2(pi*k + arctan[(a +/- sqrt(a^2+b^2-c^2))/(b-c)] where k is an integer. Also has a solution using the 2-argument arctan.
 *
 *   0 = L^2-q^2-r^2-x0^2-y0^2-(D0+s-z0)^2+2 r y0 Cos[w] - 2 q Cos[c+t u] (x0 Cos[b]+((D0+s-z0) Cos[a]+(y0-r Cos[w]) Sin[a]) Sin[b]) + 2 q (Cos[a] (-y0+r Cos[w])+(D0+s-z0) Sin[a]) Sin[c+t u] + 2 r x0 Sin[w] + 2 r q Cos[b] Sin[w] Cos[c+t u]
 *
 *   0 = L^2-q^2-r^2-x0^2-y0^2-(D0+s-z0)^2+2 r y0 Cos[w] + 2 r x0 Sin[w] + Sin[c+t u]*2 q (Cos[a] (-y0+r Cos[w])+(D0+s-z0) Sin[a]) + Cos[c+t u](-2 q (x0 Cos[b]+((D0+s-z0) Cos[a]+(y0-r Cos[w]) Sin[a]) Sin[b]) + 2 r q Cos[b] Sin[w])
 *
 *   Rewrite the above as 0 = {m,n,p} . {Sin[c+t u], Cos[c+t u], 1}
 *
 *   Thus, {m,n,p} = {2 q (Cos[a] (-y0+r Cos[w])+(D0+s-z0) Sin[a]), (-2 q (x0 Cos[b]+((D0+s-z0) Cos[a]+(y0-r Cos[w]) Sin[a]) Sin[b]) + 2 r q Cos[b] Sin[w]), L^2-q^2-r^2-x0^2-y0^2-(D0+s-z0)^2+2 r y0 Cos[w] + 2 r x0 Sin[w]} 
 *   Note: 
 *        c+t*u = arctan((-n*p - m*sqrt(m*m+n*n-p*p))/(m*m+n*n),  (-m*p + n*sqrt(m*m+n*n-p*p))/(m*m + n*n)  )  where ArcTan[x, y] = atan(y/x)
 *     OR c+t*u = arctan((-n*p + m*sqrt(m*m+n*n-p*p))/(m*m+n*n),  (-m*p - n*sqrt(m*m+n*n-p*p))/(m*m + n*n)  )
 *
 *
 *
 * Arcs - Revision 2:
 *   a circle in 2d is x(t) = rcos(wt), y(t) = rsin(wt)
 *   can write this as P(t) = rcos(wt)*i + rsin(wt)*j
 *   Replace i and j with perpindicular vectors to extend to multiple dimensions:
 *   P(t) = <xc, yc, zc> + rcos(wt)*u+ rsin(wt)*v
 *   Let x0, y0, z0 be P(0) (the starting point), and P(end) = Pe=<xe, ye, ze>, and Pc=<xc, yc, zc> will be the center of the arc.
 *   The u is just <x0-xc, y0-yc, z0-zc>.
 *
 *   Now need to solve for v. v will be in the plane containing vectors u and Pe-Pc and will be at a 90* angle with u.
 *   In other words, v is some linear combination of u and (Pe-Pc) such that u is perpindicular to v and has a magnitude of r.
 *   { v = a*u + b*(Pe-Pc), v . u = 0, |v| = r }
 *     u . (a*u + b*(Pe-Pc)) == 0
 *     a|u|^2 + b|u . (Pe-Pc)| == 0
 *   let b = 1, and we can solve for the direction of v:
 *     b = 1
 *     a = -u.(Pe-Pc) / |u|^2
 *   Then normalize v and scale it up to |u|.
 *
 *   Given u, v, Pc, and let m be the angular velocity:
 *     x = xc + r*Cos[m*t]*ux + r*Sin[m*t]*vx
 *     y = yc + r*Cos[m*t]*uy + r*Sin[m*t]*vy
 *     z = zc + r*Cos[m*t]*uz + r*Sin[m*t]*vz
 *   Substitute this information into the above derived D = z + sqrt(L^2 - (y-rcos(w))^2 - (x-rsin(w))^2), where w is tower angle and D is some axis coordinate.
 *     D(t) = (zc + r*Cos[m*t]*uz + r*Sin[m*t]*vz) + Sqrt[L^2 - ((yc + r*Cos[m*t]*uy + r*Sin[m*t]*vy) - r*Cos[w])^2 - ((xc + r*Cos[m*t]*ux + r*Sin[m*t]*vx) - r*Sin[w])^2]
 *   Want to solve for the time at which D = D0 + s, where s is a constant step offset to test:
 *     D0 + s = (zc + r*Cos[m*t]*uz + r*Sin[m*t]*vz) + Sqrt[L^2 - ((yc + r*Cos[m*t]*uy + r*Sin[m*t]*vy) - r*Cos[w])^2 - ((xc + r*Cos[m*t]*ux + r*Sin[m*t]*vx) - r*Sin[w])^2]
 *     ((D0 + s) - (zc + r*Cos[m*t]*uz + r*Sin[m*t]*vz))^2 = L^2 - ((yc + r*Cos[m*t]*uy + r*Sin[m*t]*vy) - r*Cos[w])^2 - ((xc + r*Cos[m*t]*ux + r*Sin[m*t]*vx) - r*Sin[w])^2
 *   0 = L^2 - ((yc + r*Cos[m*t]*uy + r*Sin[m*t]*vy) - r*Cos[w])^2 - ((xc + r*Cos[m*t]*ux + r*Sin[m*t]*vx) - r*Sin[w])^2 - ((D0 + s) - (zc + r*Cos[m*t]*uz + r*Sin[m*t]*vz))^2
 *
 *   Expand applied to above gives:
 *     0 = -D0^2+L^2-2 D0 s-s^2-xc^2-yc^2+2 D0 zc+2 s zc-zc^2+2 D0 r uz Cos[m t]+2 r s uz Cos[m t]-2 r ux xc Cos[m t]-2 r uy yc Cos[m t]-2 r uz zc Cos[m t]-r^2 ux^2 Cos[m t]^2-r^2 uy^2 Cos[m t]^2-r^2 uz^2 Cos[m t]^2+2 r yc Cos[w]+2 r^2 uy Cos[m t] Cos[w]-r^2 Cos[w]^2+2 D0 r vz Sin[m t]+2 r s vz Sin[m t]-2 r vx xc Sin[m t]-2 r vy yc Sin[m t]-2 r vz zc Sin[m t]-2 r^2 ux vx Cos[m t] Sin[m t]-2 r^2 uy vy Cos[m t] Sin[m t]-2 r^2 uz vz Cos[m t] Sin[m t]+2 r^2 vy Cos[w] Sin[m t]-r^2 vx^2 Sin[m t]^2-r^2 vy^2 Sin[m t]^2-r^2 vz^2 Sin[m t]^2+2 r xc Sin[w]+2 r^2 ux Cos[m t] Sin[w]+2 r^2 vx Sin[m t] Sin[w]-r^2 Sin[w]^2
 *
 *   FullSimplify applied above gives:
 *     0 = 1/2 (2 L^2-r^2 (2+vx^2+vy^2+vz^2)-2 (xc^2+yc^2)-2 (D0+s-zc)^2+r (-2 r (ux^2+uy^2+uz^2) Cos[m t]^2+r (vx^2+vy^2+vz^2) Cos[2 m t]-2 r (ux vx+uy vy+uz vz) Sin[2 m t]+4 Cos[m t] (-ux xc-uy yc+uz (D0+s-zc)+r uy Cos[w]+r ux Sin[w])+4 Sin[m t] (-vx xc-vy yc+vz (D0+s-zc)+r vy Cos[w]+r vx Sin[w])+4 (yc Cos[w]+xc Sin[w])))
 *     0 = 2 L^2-r^2 (2+vx^2+vy^2+vz^2)-2 (xc^2+yc^2)-2 (D0+s-zc)^2+r (-2 r (ux^2+uy^2+uz^2) Cos[m t]^2+r (vx^2+vy^2+vz^2) Cos[2 m t]-2 r (ux vx+uy vy+uz vz) Sin[2 m t]+4 Cos[m t] (-ux xc-uy yc+uz (D0+s-zc)+r uy Cos[w]+r ux Sin[w])+4 Sin[m t] (-vx xc-vy yc+vz (D0+s-zc)+r vy Cos[w]+r vx Sin[w])+4 (yc Cos[w]+xc Sin[w]))
 *   TrigExpand applied above gives:
 *     0 = -D0^2+L^2-r^2-2 D0 s-s^2-(r^2 ux^2)/2-(r^2 uy^2)/2-(r^2 uz^2)/2-(r^2 vx^2)/2-(r^2 vy^2)/2-(r^2 vz^2)/2-xc^2-yc^2+2 D0 zc+2 s zc-zc^2+2 D0 r uz Cos[m t]+2 r s uz Cos[m t]-2 r ux xc Cos[m t]-2 r uy yc Cos[m t]-2 r uz zc Cos[m t]-1/2 r^2 ux^2 Cos[m t]^2-1/2 r^2 uy^2 Cos[m t]^2-1/2 r^2 uz^2 Cos[m t]^2+1/2 r^2 vx^2 Cos[m t]^2+1/2 r^2 vy^2 Cos[m t]^2+1/2 r^2 vz^2 Cos[m t]^2+2 r yc Cos[w]+2 r^2 uy Cos[m t] Cos[w]+2 D0 r vz Sin[m t]+2 r s vz Sin[m t]-2 r vx xc Sin[m t]-2 r vy yc Sin[m t]-2 r vz zc Sin[m t]-2 r^2 ux vx Cos[m t] Sin[m t]-2 r^2 uy vy Cos[m t] Sin[m t]-2 r^2 uz vz Cos[m t] Sin[m t]+2 r^2 vy Cos[w] Sin[m t]+1/2 r^2 ux^2 Sin[m t]^2+1/2 r^2 uy^2 Sin[m t]^2+1/2 r^2 uz^2 Sin[m t]^2-1/2 r^2 vx^2 Sin[m t]^2-1/2 r^2 vy^2 Sin[m t]^2-1/2 r^2 vz^2 Sin[m t]^2+2 r xc Sin[w]+2 r^2 ux Cos[m t] Sin[w]+2 r^2 vx Sin[m t] Sin[w]
 *   Note: cos(2x) = cos^2(x) - sin^2(x) = 2cos^2(x)-1
 *   Therefore: cos^2(x) = (cos(2x)+1)/2 and sin^2(x) = (cos(2x)-1)/2
 *   Thus,
 *     0 = 2 L^2-r^2 (2+vx^2+vy^2+vz^2)-2 (xc^2+yc^2)-2 (D0+s-zc)^2+r (-2 r (ux^2+uy^2+uz^2) (Cos[2m t]+1)/2+r (vx^2+vy^2+vz^2) Cos[2 m t]-2 r (ux vx+uy vy+uz vz) Sin[2 m t]+4 Cos[m t] (-ux xc-uy yc+uz (D0+s-zc)+r uy Cos[w]+r ux Sin[w])+4 Sin[m t] (-vx xc-vy yc+vz (D0+s-zc)+r vy Cos[w]+r vx Sin[w])+4 (yc Cos[w]+xc Sin[w]))
 *
 *   Knowing that |v| = |u| = r, can reduce to:
 *   0 = 2L^2 - 2r^2 - r^4 - 2|<xc, yc, D0+s-zc>|^2 - r^4*Sin[m t]^2 + 4r<r*Sin[w]-xc, r*Cos[w]-yc, D0+s-zc> . (u*Cos[m t] + v*Sin[m t]) + 4r*(yc*Cos[w] + xc*Sin[w])
 *
 *
 *
 * Arcs - Revision 3:
 *   Use the same parameterization as in Rev 2, but do more solving by hand.
 *   Let P = P(t) = <xc, yc, zc> + s*cos(m*t)u + s*sin(m*t)v where s is the radius of the curve
 *   Have the constraint equation: |P - <rsin(w), rcos(w), D>| = L, where <rsin(w), rcos(w), D> is the carriage position and P is the effector position
 *   Then substitute P into the constraint equation and square each side:
 *     L^2 = |<xc-rsin(w), yc-rcos(w), zc-D> + s*cos(m*t)u + s*sin(m*t)v|^2
 *   Manually expand based upon the property that |v|^2 = v . v
 *     (xc-rsin(w))^2 + (yc-rcos(w))^2 + (zc-D)^2 + s^2*cos^2(m*t) + s^2*sin^2(m*t) + 2<xc-rsin(w), yc-rcos(w), zc-D> . s*cos(m*t)u + 2<xc-rsin(w), yc-rcos(w), zc-D> . s*sin(m*t)v == L^2
 *   Note: removed all terms involving u . v, because u is perpindicular to v so u.v = 0
 *   Can manually simplify a bit and put into Mathematica notation. Note: used mt=m*t to make solving for t slightly easier:
 *     (xc-r*Sin[w])^2 + (yc-r*Cos[w])^2 + (zc-D)^2 + s^2 + 2*s*{xc-r*Sin[w], yc-r*Cos[w], zc-D} . (Cos[mt]*u + Sin[mt]*v) == L^2
 *   Can directly apply Solve on the above equation and mt, but produces LARGE output. So apply FullSimplify on the above (with u->{ux, uy, uz}, v->{vx, vy, vz}):
 *     s^2+(D-zc)^2+(yc-r Cos[w])^2+(xc-r Sin[w])^2+2 s ((yc-r Cos[w]) (uy Cos[mt]+vy Sin[mt])+(-D+zc) (uz Cos[mt]+vz Sin[mt])+(ux Cos[mt]+vx Sin[mt]) (xc-r Sin[w])) == L^2
 *   Expand:
 *     0 == D^2-L^2+s^2+xc^2+yc^2-2 D zc+zc^2-2 D s uz Cos[mt]+2 s ux xc Cos[mt]+2 s uy yc Cos[mt]+2 s uz zc Cos[mt]-2 r yc Cos[w]-2 r s uy Cos[mt] Cos[w]+r^2 Cos[w]^2-2 D s vz Sin[mt]+2 s vx xc Sin[mt]+2 s vy yc Sin[mt]+2 s vz zc Sin[mt]-2 r s vy Cos[w] Sin[mt]-2 r xc Sin[w]-2 r s ux Cos[mt] Sin[w]-2 r s vx Sin[mt] Sin[w]+r^2 Sin[w]^2
 *   Apply Collect[%, {Sin[mt], Cos[mt]}] to above to group terms:
 *   0 ==  D^2-L^2+s^2+xc^2+yc^2-2 D zc+zc^2-2 r yc Cos[w]+r^2 Cos[w]^2-2 r xc Sin[w]+r^2 Sin[w]^2
     + Cos[mt] (-2 D s uz+2 s ux xc+2 s uy yc+2 s uz zc-2 r s uy Cos[w]-2 r s ux Sin[w])
     + Sin[mt] (-2 D s vz+2 s vx xc+2 s vy yc+2 s vz zc-2 r s vy Cos[w]-2 r s vx Sin[w])
 *   apply FullSimplify to each term:
 *   0 == -L^2+r^2+s^2+xc^2+yc^2+(D-zc)^2-2 r (yc Cos[w]+xc Sin[w])
     + Cos[mt] (2 s (-D uz+ux xc+uy yc+uz zc-r (uy Cos[w]+ux Sin[w])))
     + Sin[mt] (2 s (-D vz+vx xc+vy yc+vz zc-r (vy Cos[w]+vx Sin[w])))
 *   Now we can use the earlier derived identity: {m,n,p} . {Sin[mt], Cos[mt], 1} has solutions of:
 *        mt = arctan((-n*p - m*sqrt(m*m+n*n-p*p))/(m*m+n*n),  (-m*p + n*sqrt(m*m+n*n-p*p))/(m*m + n*n)  )  where ArcTan[x, y] = atan(y/x)
 *     OR mt = arctan((-n*p + m*sqrt(m*m+n*n-p*p))/(m*m+n*n),  (-m*p - n*sqrt(m*m+n*n-p*p))/(m*m + n*n)  )
 *   Note: D=D0+s
 */


#ifndef DRIVERS_LINEARDELTASTEPPER_H
#define DRIVERS_LINEARDELTASTEPPER_H

#include "axisstepper.h"
#include "linearstepper.h" //for LinearHomeStepper
#include "lineardeltacoordmap.h" //for DeltaAxis
#include "endstop.h"
#include "common/logging.h"

namespace drv {

template <DeltaAxis AxisIdx> class LinearDeltaArcStepper : public AxisStepper {
    private:
        float _r, _L, _MM_STEPS; //settings which will be obtained from the CoordMap
        float M0; //initial coordinate of THIS axis.
        int sTotal; //current step offset from M0
        float xc, yc, zc, ux, uy, uz, vx, vy, vz;
        float arcRad; //radius of arc
        float m;
        float w; //angle of this axis. CW from +y axis
        float x0, y0, z0; //center point of arc
        float r() const { return _r; }
        float L() const { return _L; }
        float MM_STEPS() const { return _MM_STEPS; }
    public:
        LinearDeltaArcStepper() : _r(0), _L(0), _MM_STEPS(0) {}
        template <typename CoordMapT, std::size_t sz> LinearDeltaArcStepper(int idx, const CoordMapT &map, const std::array<int, sz> &curPos, float xCenter, float yCenter, float zCenter, float ux, float uy, float uz, float vx, float vy, float vz, float arcRad, float arcVel, float extVel)
          : AxisStepper(idx),
            _r(map.r()),
            _L(map.L()),
            _MM_STEPS(map.MM_STEPS(AxisIdx)),
            M0(map.getAxisPosition(curPos, AxisIdx)*map.MM_STEPS(AxisIdx)), 
            sTotal(0),
            xc(xCenter),
            yc(yCenter),
            zc(zCenter),
            ux(ux),
            uy(uy),
            uz(uz),
            vx(vx),
            vy(vy),
            vz(vz),
            arcRad(arcRad),
            m(arcVel),
            w(AxisIdx*2*M_PI/3) {
                (void)map, (void)idx; (void)extVel; //unused
                static_assert(AxisIdx < 3, "LinearDeltaStepper only supports axis A, B, or C (0, 1, 2)");
                this->time = 0; //this may NOT be zero-initialized by parent.
        }
    //protected:
        float testDir(float s, float curTime) {
            //{m, n, p} = {2 q (Cos[a] (-y0+r Cos[w])+(D0+s-z0) Sin[a]), -2 q (x0 Cos[b]+((D0+s-z0) Cos[a]+(y0-r Cos[w]) Sin[a]) Sin[b]) + 2 r q Cos[b] Sin[w], L^2-q^2-r^2-x0^2-y0^2-(D0+s-z0)^2+2 r y0 Cos[w]}
            //float m = 2*arcRad*(M0+s)*sin(a) + 2*arcRad*cos(a)*r()*cos(w);
            //float n = 2*arcRad*cos(a)*-(M0+s)*sin(b) + 2*arcRad*r()*(cos(w)*sin(a)*sin(b) + cos(b)*sin(w));
            //float p = L()*L() - arcRad*arcRad - r()*r() - (M0+s)*(M0+s);
            
            //float m = 2*arcRad*(cos(a)*(-y0+r()*cos(w))+(M0+s-z0)*sin(a));
            //float n = -2*arcRad*(x0*cos(b)+((M0+s-z0)*cos(a)+(y0-r()*cos(w))*sin(a))*sin(b)) + 2*r()*arcRad*cos(b)*sin(w);
            //float p = L()*L() - arcRad*arcRad - r()*r() - x0*x0 - y0*y0 - (M0+s-z0)*(M0+s-z0) + 2*r()*y0*cos(w)  + 2*r()*x0*sin(w);
            
            //float c_tu = atan2((-m*sqrt(m*m+n*n-p*p)-n*p)/(m*m+n*n), (n*sqrt(m*m+n*n-p*p) + n*n*p/m)/(m*m+n*n)-p/m); // OR c+t*u = arctan((m*sqrt(m^2+n^2-p^2)-np)/(m^2+n^2), (-n*sqrt(m^2+n^2-p^2) + n^2*p/m)/(m^2+n^2)-p/m);
            //if (c_tu > M_PI/4) { //rounding errors
            //    c_tu -= M_PI/2;
            //}
            //float t = (c_tu - c)/u;
            //return t;
            
            float D = M0+s;
            //        r^2      +s^2          +xc^2 +yc^2 +(D-zc)^2     -2 r   (yc Cos[w]+xc Sin[w]) - L^2
            float p = r()*r()  +arcRad*arcRad+xc*xc+yc*yc+(D-zc)*(D-zc)-2*r()*(yc*cos(w)+xc*sin(w)) - L()*L();
            //        2 s      (-D uz+ux xc+uy yc+uz zc-r   (uy Cos[w]+ux Sin[w]))
            float n = 2*arcRad*(-D*uz+ux*xc+uy*yc+uz*zc-r()*(uy*cos(w)+ux*sin(w)));
            //        2 s      (-D vz+vx xc+vy yc+vz zc-r   (vy Cos[w]+vx Sin[w]))
            float m = 2*arcRad*(-D*vz+vx*xc+vy*yc+vz*zc-r()*(vy*cos(w)+vx*sin(w)));
            
            float mt_1 = atan2((-m*p + n*sqrt(m*m+n*n-p*p))/(m*m + n*n), (-n*p - m*sqrt(m*m+n*n-p*p))/(m*m+n*n));
            float mt_2 = atan2((-m*p - n*sqrt(m*m+n*n-p*p))/(m*m + n*n), (-n*p + m*sqrt(m*m+n*n-p*p))/(m*m+n*n));
            float t1 = mt_1/this->m;
            float t2 = mt_2/this->m;
            if (t1 < curTime && t2 < curTime) { return NAN; }
            else if (t1 < curTime) { return t2; }
            else if (t2 < curTime) { return t1; }
            else { return std::min(t1, t2); }
        }
        void _nextStep() {
            //called to set this->time and this->direction; the time (in seconds) and the direction at which the next step should occur for this axis
            //General formula is outlined in comments at the top of this file.
            //First, we test the time at which a forward step (sTotal + 1) should occur given constant angular velocity.
            //Then we test that time for a backward step (sTotal - 1).
            //We choose the nearest resulting time as our next step.
            //This is necessary because axis velocity can actually reverse direction during a circular cartesian movement.
            float negTime = testDir((this->sTotal-1)*MM_STEPS(), this->time); //get the time at which next steps would occur.
            float posTime = testDir((this->sTotal+1)*MM_STEPS(), this->time);
            if (negTime < this->time || std::isnan(negTime)) { //negTime is invalid
                if (posTime > this->time) {
                    LOGV("LinearDeltaArcStepper<%zu>::chose %f (pos) vs %f (neg)\n", AxisIdx, posTime, negTime);
                    this->time = posTime;
                    this->direction = StepForward;
                    ++this->sTotal;
                } else {
                    this->time = NAN;
                }
            } else if (posTime < this->time || std::isnan(posTime)) { //posTime is invalid
                if (negTime > this->time) {
                    LOGV("LinearDeltaArcStepper<%zu>::chose %f (neg) vs %f (pos)\n", AxisIdx, negTime, posTime);
                    this->time = negTime;
                    this->direction = StepBackward;
                    --this->sTotal;
                } else {
                    this->time = NAN;
                }
            } else { //neither time is invalid
                if (negTime < posTime) {
                    LOGV("LinearDeltaArcStepper<%zu>::chose %f (neg) vs %f (pos)\n", AxisIdx, negTime, posTime);
                    this->time = negTime;
                    this->direction = StepBackward;
                    --this->sTotal;
                } else {
                    LOGV("LinearDeltaArcStepper<%zu>::chose %f (pos) vs %f (neg)\n", AxisIdx, posTime, negTime);
                    this->time = posTime;
                    this->direction = StepForward;
                    ++this->sTotal;
                }
            }
        }
};

template <DeltaAxis AxisIdx, typename EndstopT=EndstopNoExist> class LinearDeltaStepper : public AxisStepper {
    private:
        float _r, _L, _MM_STEPS; //settings which will be obtained from the CoordMap
        float M0; //initial coordinate of THIS axis.
        int sTotal; //current step offset from M0
        float inv_v2; //1/v^2, where v is the linear speed in cartesian-space
        float vz_over_v2; //vz/v^2, where vz is the 
        float _almostTerm1; //used for caching & reducing computational complexity inside nextStep()
        float _almostRootParam;
        float _almostRootParamV2S;
        float r() const { return _r; }
        float L() const { return _L; }
        float MM_STEPS() const { return _MM_STEPS; }
    public:
        typedef LinearHomeStepper<AxisIdx, EndstopT> HomeStepperT;
        typedef LinearDeltaArcStepper<AxisIdx> ArcStepperT;
        LinearDeltaStepper() : _r(0), _L(0), _MM_STEPS(0) {}
        template <typename CoordMapT, std::size_t sz> LinearDeltaStepper(int idx, const CoordMapT &map, const std::array<int, sz>& curPos, float vx, float vy, float vz, float ve)
           : AxisStepper(idx),
             _r(map.r()), _L(map.L()), _MM_STEPS(map.MM_STEPS(AxisIdx)),
             M0(map.getAxisPosition(curPos, AxisIdx)*map.MM_STEPS(AxisIdx)), 
             sTotal(0),
             //vx(vx), vy(vy), vz(vz),
             //v2(vx*vx + vy*vy + vz*vz), 
             inv_v2(1/(vx*vx + vy*vy + vz*vz)),
             vz_over_v2(vz*inv_v2) {
                (void)ve; //unused
                static_assert(AxisIdx < 3, "LinearDeltaStepper only supports axis A, B, or C (0, 1, 2)");
                this->time = 0; //this may NOT be zero-initialized by parent.
                float x0, y0, z0, e_;
                //map.xyzeFromMechanical(curPos, this->x0, this->y0, this->z0, e_);
                std::tie(x0, y0, z0, e_) = map.xyzeFromMechanical(curPos);
                //precompute as much as possible:
                _almostRootParamV2S = 2*M0 - 2*z0;
                if (AxisIdx == DELTA_AXIS_A) {
                    _almostTerm1 = inv_v2*(r()*vy - vx*x0 - vy*y0 + vz*(M0 - z0)); // + vz/v2*s;
                    //rootParam = term1*term1 - v2*(-L()*L() + x0*x0 + (r() - y0)*(r() - y0) + (M0 + s - z0)*(M0 + s - z0));
                    //rootParam = term1*term1 - v2*(-L()*L() + x0*x0 + (r() - y0)*(r() - y0) + M0*M0 + 2*M0*s - 2*M0*z0 + s*s - 2*s*z0 + z0*z0);
                    //rootParam = term1*term1 - v2*(-L()*L() + x0*x0 + (r() - y0)*(r() - y0) + M0*M0 - 2*M0*z0 + z0*z0) - v2*s*(2*M0 + s - 2*z0);
                    _almostRootParam = -inv_v2*(-L()*L() + x0*x0 + (r() - y0)*(r() - y0) + M0*M0 - 2*M0*z0 + z0*z0);
                    //_almostRootParamV2S = 2*M0 - 2*z0; // (...+s)*-1/v2*s
                } else if (AxisIdx == DELTA_AXIS_B) { 
                    _almostTerm1 = inv_v2*(r()*(sqrt(3)*vx - vy)/2. - vx*x0 - vy*y0 + vz*(M0 - z0)); // + vz/v2*s;
                    //rootParam = term1*term1 - v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(-sqrt(3)*x0 + y0) + (M0 + s - z0)*(M0 + s - z0));
                    _almostRootParam = -inv_v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(-sqrt(3)*x0 + y0) + M0*M0 - 2*M0*z0 + z0*z0);
                    //_almostRootParamV2S = 2*M0 - 2*z0;
                } else if (AxisIdx == DELTA_AXIS_C) {
                    _almostTerm1 = inv_v2*(-r()*(sqrt(3)*vx + vy)/2 - vx*x0 - vy*y0 + vz*(M0 - z0)); // + vz/v2*s;
                    //rootParam = term1*term1 - v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(sqrt(3)*x0 + y0) + (M0 + s - z0)*(M0 + s - z0));
                    _almostRootParam = -inv_v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(sqrt(3)*x0 + y0) + M0*M0 - 2*M0*z0 + z0*z0);
                    //_almostRootParamV2S = 2*M0 - 2*z0;
                }
            }
        void getTerm1AndRootParam(float &term1, float &rootParam, float s) {
            //Therefore, we should cache values calculatable at init-time, like all of the second-half on rootParam.
            term1 = _almostTerm1 + vz_over_v2*s;
            rootParam = term1*term1 + _almostRootParam - inv_v2*s*(_almostRootParamV2S + s);
            /*if (AxisIdx == 0) {
                term1 = r()*vy - vx*x0 - vy*y0 + vz*(M0 + s - z0);
                rootParam = term1*term1 - v2*(-L()*L() + x0*x0 + (r() - y0)*(r() - y0) + (M0 + s - z0)*(M0 + s - z0));
            } else if (AxisIdx == 1) { 
                term1 = r()*(sqrt(3)*vx - vy)/2. - vx*x0 - vy*y0 + vz*(M0 + s - z0);
                rootParam = term1*term1 - v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(-sqrt(3)*x0 + y0) + (M0 + s - z0)*(M0 + s - z0));
            } else if (AxisIdx == 2) {
                term1 = -r()*(sqrt(3)*vx + vy)/2 - vx*x0 - vy*y0 + vz*(M0 + s - z0);
                rootParam = term1*term1 - v2*(-L()*L() + r()*r() + x0*x0 + y0*y0 + r()*(sqrt(3)*x0 + y0) + (M0 + s - z0)*(M0 + s - z0));
            }*/
            /*float root = std::sqrt(rootParam);
            float t1 = (term1 - root)/v2;
            float t2 = (term1 + root)/v2;*/
        }
        float testDir(float s) {
            float term1, rootParam;
            getTerm1AndRootParam(term1, rootParam, s);
            if (rootParam < 0) {
                return NAN;
            }
            float root = std::sqrt(rootParam);
            float t1 = term1 - root;
            float t2 = term1 + root;
            if (root > term1) { //if this is true, then t1 MUST be negative.
                //return t2 if t2 > last_step_time else None
                return t2 > this->time ? t2 : NAN;
            } else {
                return t1 > this->time ? t1 : (t2 > this->time ? t2 : NAN); //ensure no value < time is returned.
            }
        }
        void _nextStep() {
            //called to set this->time and this->direction; the time (in seconds) and the direction at which the next step should occur for this axis
            //General formula is outlined in comments at the top of this file.
            //First, we test the time at which a forward step (sTotal + 1) should occur given constant cartesian velocity.
            //Then we test that time for a backward step (sTotal - 1).
            //We choose the nearest resulting time as our next step.
            //This is necessary because axis velocity can actually reverse direction during a linear cartesian movement.
            float negTime = testDir((this->sTotal-1)*MM_STEPS()); //get the time at which next steps would occur.
            float posTime = testDir((this->sTotal+1)*MM_STEPS());
            if (negTime < this->time || std::isnan(negTime)) { //negTime is invalid
                if (posTime > this->time) {
                    //LOGV("LinearDeltaStepper<%zu>::chose %f (pos)\n", AxisIdx, posTime);
                    this->time = posTime;
                    this->direction = StepForward;
                    ++this->sTotal;
                } else {
                    this->time = NAN;
                }
            } else if (posTime < this->time || std::isnan(posTime)) { //posTime is invalid
                if (negTime > this->time) {
                    //LOGV("LinearDeltaStepper<%zu>::chose %f (neg)\n", AxisIdx, negTime);
                    this->time = negTime;
                    this->direction = StepBackward;
                    --this->sTotal;
                } else {
                    this->time = NAN;
                }
            } else { //neither time is invalid
                if (negTime < posTime) {
                    //LOGV("LinearDeltaStepper<%zu>::chose %f (neg)\n", AxisIdx, negTime);
                    this->time = negTime;
                    this->direction = StepBackward;
                    --this->sTotal;
                } else {
                    //LOGV("LinearDeltaStepper<%zu>::chose %f (pos)\n", AxisIdx, posTime);
                    this->time = posTime;
                    this->direction = StepForward;
                    ++this->sTotal;
                }
            }
        }
};

}


#endif

