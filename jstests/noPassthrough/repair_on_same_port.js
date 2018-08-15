(function() {
    'use strict';

    //var mongo = MongoRunner.runMongod({port: 27018});
    var mongoRepair = MongoRunner.runMongod({port: 27018, repair: ""});


    //MongoRunner.stopMongod(mongo);
})();