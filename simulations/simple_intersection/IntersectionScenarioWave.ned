package simple_intersection;

import org.car2x.veins.nodes.RSU;
import org.car2x.veins.nodes.Scenario;
import org.car2x.veins.modules.obstacle.VehicleObstacleControl;

network IntersectionScenarioWave extends Scenario
{
    parameters:
        @display("bgb=800,600");
    submodules:
        vehicleObstacles: VehicleObstacleControl {
            @display("p=320,50");
        }
}
