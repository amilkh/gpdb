select * from uao_visimap_test05;
select count(*) from gp_dist_random('pg_aoseg.pg_aovisimap_18636');
insert into uao_visimap_test05 select i,'aa'||i,i+10 from generate_series(1,5) as i;
delete from uao_visimap_test05 where i=3;
select * from uao_visimap_test05;
select count(*) from gp_dist_random('pg_aoseg.pg_aovisimap_18636');
truncate table uao_visimap_test05;
select * from uao_visimap_test05;
select count(*) from gp_dist_random('pg_aoseg.pg_aovisimap_18636');
